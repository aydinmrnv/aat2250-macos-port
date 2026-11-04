/*
 * VMDProbe - phase 0 of a macOS driver for Intel VMD 8086:AD0B (Arrow Lake).
 *
 * READ-ONLY. It never writes PCI config space or MMIO. It maps the VMD CFGBAR (ECAM of the VMD
 * domain) read-only, walks the bus window and publishes what it finds as the IORegistry property
 * "VMDTopology", so the phase-1 host bridge can be designed from facts instead of guesses.
 *
 * Opt-in: does nothing unless the boot-arg -vmdprobe is present; -vmdoff always disables it.
 * Register facts (VMCAP 0x40, VMCONFIG 0x44, bus window, MSI-X) follow FreeBSD sys/dev/vmd/vmd.c
 * (BSD-2-Clause) and are cross-checked against Linux drivers/pci/controller/vmd.c (no code copied).
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

#define VMD_LOG(fmt, ...) IOLog("VMDProbe: " fmt "\n", ##__VA_ARGS__)

enum {
    kVMDVendorIntel   = 0x8086,
    kVMDDeviceAD0B    = 0xAD0B,
    kVMDRegVMCAP      = 0x40,   /* bit 0: bus restriction supported */
    kVMDRegVMCONFIG   = 0x44,   /* bits 9:8: start bus 0 / 128 / 224 */
    kVMDMaxFunctions  = 64,     /* bound the published property */
    kVMDMaxCaps       = 48,     /* bound the capability walk (loops in a broken list) */
};

class VMDProbe : public IOService {
    OSDeclareDefaultStructors(VMDProbe)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
private:
    static uint32_t rd32(volatile uint8_t *base, uint64_t off) { return *(volatile uint32_t *)(base + off); }
    OSDictionary *describe(volatile uint8_t *fn, unsigned bus, unsigned dev, unsigned func);
    static void putNum(OSDictionary *d, const char *key, uint64_t v, unsigned bits);
};

OSDefineMetaClassAndStructors(VMDProbe, IOService)

void VMDProbe::putNum(OSDictionary *d, const char *key, uint64_t v, unsigned bits)
{
    OSNumber *n = OSNumber::withNumber(v, bits);
    if (n) { d->setObject(key, n); n->release(); }
}

IOService *VMDProbe::probe(IOService *provider, SInt32 *score)
{
    int flag = 0;
    if (PE_parse_boot_argn("-vmdoff", &flag, sizeof flag)) return nullptr;
    if (!PE_parse_boot_argn("-vmdprobe", &flag, sizeof flag)) return nullptr;
    return IOService::probe(provider, score);
}

/* One function's identity, BARs and capability IDs, read from its 4 KB ECAM window. */
OSDictionary *VMDProbe::describe(volatile uint8_t *fn, unsigned bus, unsigned dev, unsigned func)
{
    OSDictionary *d = OSDictionary::withCapacity(12);
    if (!d) return nullptr;
    const uint32_t id = rd32(fn, 0x00), cls = rd32(fn, 0x08), hdr = (rd32(fn, 0x0C) >> 16) & 0xFF;
    const uint32_t sts = rd32(fn, 0x04) >> 16;
    putNum(d, "bus", bus, 8); putNum(d, "device", dev, 8); putNum(d, "function", func, 8);
    putNum(d, "vendor-id", id & 0xFFFF, 16); putNum(d, "device-id", id >> 16, 16);
    putNum(d, "class-code", cls >> 8, 32); putNum(d, "header-type", hdr, 8);
    putNum(d, "command", rd32(fn, 0x04) & 0xFFFF, 16);

    const unsigned nbars = (hdr & 0x7F) == 1 ? 2 : (hdr & 0x7F) == 0 ? 6 : 0;
    OSArray *bars = OSArray::withCapacity(nbars ? nbars : 1);
    for (unsigned i = 0; bars && i < nbars; i++) {
        OSNumber *n = OSNumber::withNumber(rd32(fn, 0x10 + 4 * i), 32);
        if (n) { bars->setObject(n); n->release(); }
    }
    if (bars) { d->setObject("bars", bars); bars->release(); }
    if ((hdr & 0x7F) == 1) {
        const uint32_t b = rd32(fn, 0x18);
        putNum(d, "primary-bus", b & 0xFF, 8); putNum(d, "secondary-bus", (b >> 8) & 0xFF, 8);
        putNum(d, "subordinate-bus", (b >> 16) & 0xFF, 8);
    }

    OSArray *caps = OSArray::withCapacity(8);
    if (caps && (sts & 0x10)) {                                  /* status: capabilities list */
        unsigned ptr = rd32(fn, 0x34) & 0xFC;
        for (unsigned n = 0; ptr >= 0x40 && n < kVMDMaxCaps; n++) {
            const uint32_t c = rd32(fn, ptr);
            OSNumber *num = OSNumber::withNumber(((uint64_t)ptr << 8) | (c & 0xFF), 32);  /* offset<<8 | id */
            if (num) { caps->setObject(num); num->release(); }
            ptr = (c >> 8) & 0xFC;
        }
    }
    if (caps) { d->setObject("capabilities", caps); caps->release(); }

    VMD_LOG("%02x:%02x.%u %04x:%04x class %06x hdr %02x cmd %04x", bus, dev, func, id & 0xFFFF, id >> 16,
            cls >> 8, hdr, rd32(fn, 0x04) & 0xFFFF);
    return d;
}

bool VMDProbe::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;

    const uint16_t ven = pci->configRead16(kIOPCIConfigVendorID), dev = pci->configRead16(kIOPCIConfigDeviceID);
    const uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
    setProperty("VMDCommand", cmd, 16);
    if (ven != kVMDVendorIntel || dev != kVMDDeviceAD0B) {
        setProperty("VMDStatus", "unexpected device id; not scanning");
        return true;
    }

    /* Bus window, as FreeBSD/Linux derive it. */
    const uint16_t vmcap = pci->configRead16(kVMDRegVMCAP), vmcfg = pci->configRead16(kVMDRegVMCONFIG);
    unsigned busStart = 0;
    if (vmcap & 0x1) {
        switch ((vmcfg >> 8) & 0x3) {
            case 0: busStart = 0; break;
            case 1: busStart = 128; break;
            default: busStart = 224; break;
        }
    }
    setProperty("VMDVMCAP", vmcap, 16);
    setProperty("VMDVMCONFIG", vmcfg, 16);
    setProperty("VMDBusStart", busStart, 8);

    UInt8 msixOff = 0;
    if (pci->findPCICapability(kIOPCICapabilityIDMSIX, &msixOff) && msixOff)
        setProperty("VMDMSIXCount", (pci->configRead16(msixOff + 2) & 0x7FF) + 1, 16);

    if (!(cmd & kIOPCICommandMemorySpace)) {
        /* Phase 0 never writes, so it will not turn decoding on. */
        setProperty("VMDStatus", "memory decode disabled by firmware/OS; not scanning");
        VMD_LOG("memory decode off (cmd %04x); not scanning", cmd);
        return true;
    }

    IODeviceMemory *cfg = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IOMemoryMap *map = cfg ? cfg->createMappingInTask(kernel_task, 0,
                                                      kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly)
                           : nullptr;
    if (!map) {
        setProperty("VMDStatus", "could not map CFGBAR");
        return true;
    }
    volatile uint8_t *base = (volatile uint8_t *)map->getVirtualAddress();
    const uint64_t size = map->getLength();
    /* v0.0.2: VMCONFIG bus-restrict encoding 3 (seen on this machine, 0x2F01) is undefined in the Linux and FreeBSD
     * drivers, and v0.0.1's 32-bus scan from 224 found nothing. Scan the whole CFGBAR (every ECAM bus slot it holds),
     * so both relative (bus - busStart) and absolute bus numbering are covered, and count what the reads return. */
    const unsigned buses = (unsigned)((size >> 20) > 256 ? 256 : (size >> 20));
    setProperty("VMDCfgBarPhys", map->getPhysicalAddress(), 64);
    setProperty("VMDCfgBarSize", size, 64);
    setProperty("VMDBusCount", buses, 16);
    setProperty("VMDBAR0Raw", pci->configRead32(kIOPCIConfigBaseAddress0), 32);
    setProperty("VMDBAR1Raw", pci->configRead32(kIOPCIConfigBaseAddress1), 32);
    setProperty("VMDBAR2Raw", pci->configRead32(kIOPCIConfigBaseAddress2), 32);
    setProperty("VMDBAR4Raw", pci->configRead32(kIOPCIConfigBaseAddress4), 32);
    VMD_LOG("VMCAP %04x VMCONFIG %04x busStart %u CFGBAR %llu MB, scanning %u ECAM buses", vmcap, vmcfg, busStart,
            size >> 20, buses);

    OSArray *topo = OSArray::withCapacity(16);
    unsigned found = 0;
    uint32_t nOnes = 0, nZero = 0, nOther = 0;
    for (unsigned b = 0; topo && b < buses && found < kVMDMaxFunctions; b++)
        for (unsigned d = 0; d < 32 && found < kVMDMaxFunctions; d++)
            for (unsigned f = 0; f < 8 && found < kVMDMaxFunctions; f++) {
                volatile uint8_t *fn = base + ((uint64_t)b << 20) + ((uint64_t)d << 15) + ((uint64_t)f << 12);
                const uint32_t id = rd32(fn, 0);
                if (id == 0xFFFFFFFFu) { nOnes++; if (f == 0) break; continue; }
                if (id == 0) { nZero++; if (f == 0) break; continue; }
                nOther++;
                /* bus reported as the ECAM slot index; busStart is published separately for interpretation */
                OSDictionary *e = describe(fn, b, d, f);
                if (e) { topo->setObject(e); e->release(); found++; }
                if (f == 0 && !((rd32(fn, 0x0C) >> 16) & 0x80)) break;   /* single-function device */
            }
    if (topo) { setProperty("VMDTopology", topo); topo->release(); }
    setProperty("VMDReadsAllOnes", nOnes, 32);
    setProperty("VMDReadsZero", nZero, 32);
    setProperty("VMDReadsData", nOther, 32);
    setProperty("VMDStatus", found ? "scanned (bus = ECAM slot index)" : "scanned; no functions found");
    VMD_LOG("%u functions published; reads: %u all-ones, %u zero, %u data", found, nOnes, nZero, nOther);

    map->release();   /* nothing stays mapped */
    return true;
}

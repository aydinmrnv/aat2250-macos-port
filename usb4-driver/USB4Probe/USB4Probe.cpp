/*
 * USB4Probe - phase D1 of a macOS connection manager for the Arrow Lake USB4 host router 8086:7EC2
 * (Linux calls this ID MTL_P_NHI0) and the WD19TB dock behind it. See deliverables/DOCK-AND-IGPU-PLAN.md.
 *
 * READ-ONLY. It never writes PCI config space or MMIO, sets up no DMA rings and changes no power state.
 * It answers the questions phase D2 has to be designed around:
 *   - Is a firmware connection manager (Intel ICM) running? If so, D2 only has to talk to it instead of being
 *     a full software CM. This is FW_STS.ICM_EN / NVM_AUTH_DONE. Linux supports ICM on this NHI when the OS
 *     has not taken USB4 control through _OSC, and macOS never asks for it.
 *   - Did firmware leave rings or a PCIe tunnel up? Ring 0 registers; the link and slot state of the TBT root
 *     ports 7EC4-7EC7 and whatever macOS enumerated below them.
 *
 * MMIO is read from a fixed allowlist only. The ring notify registers at 0x37800 and up are cleared on read
 * (Linux nhi.c), so they are never touched.
 * Register offsets and bit names: USB4 host interface as documented in Linux drivers/thunderbolt/nhi_regs.h
 * (read as a reference only; no code copied).
 *
 * Opt-in: it does nothing unless the boot-arg -usb4probe is present. -usb4off always disables it.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>

#define U4_LOG(fmt, ...) IOLog("USB4Probe: " fmt "\n", ##__VA_ARGS__)

enum {
    kVendorIntel        = 0x8086,
    kNHI7EC2            = 0x7EC2,
    kRootPortFirst      = 0x7EC4,    /* TCSS PCIe root ports (TBT PCIe tunnels end here) */
    kRootPortLast       = 0x7EC7,

    kRegTxRing0         = 0x00000,   /* 16 B per hop: base lo/hi, tail/head, size */
    kRegRxRing0         = 0x08000,
    kRegTxOptions0      = 0x19800,   /* 32 B per hop: ring flags (bit 31 = ring enabled) */
    kRegRxOptions0      = 0x29800,
    kRegCaps            = 0x39640,   /* bits 9:0 hop count, 23:16 version (0x40 = USB4 v2) */
    kRegDmaMisc         = 0x39864,
    kRegReset           = 0x39898,
    kRegInmailCmd       = 0x39904,
    kRegOutmailCmd      = 0x3990C,   /* bits 11:8 operation mode */
    kRegFwSts           = 0x39944,   /* 0 ICM_EN, 1 ICM_EN_INVERT, 2 ICM_EN_CPU, 30 CIO_RESET_REQ, 31 NVM_AUTH_DONE */
    kMinBarSize         = 0x40000,   /* every allowlisted offset lies below 256 KB */

    kMaxBelow           = 16,        /* bound the per-root-port device list */
};

static const struct { const char *name; uint32_t off; } kAllowlist[] = {
    { "TxRing0BaseLo", kRegTxRing0 + 0x0 },  { "TxRing0BaseHi", kRegTxRing0 + 0x4 },
    { "TxRing0Index",  kRegTxRing0 + 0x8 },  { "TxRing0Size",   kRegTxRing0 + 0xC },
    { "RxRing0BaseLo", kRegRxRing0 + 0x0 },  { "RxRing0BaseHi", kRegRxRing0 + 0x4 },
    { "RxRing0Index",  kRegRxRing0 + 0x8 },  { "RxRing0Size",   kRegRxRing0 + 0xC },
    { "TxRing0Flags",  kRegTxOptions0 },     { "RxRing0Flags",  kRegRxOptions0 },
    { "Caps",          kRegCaps },           { "DmaMisc",       kRegDmaMisc },
    { "Reset",         kRegReset },          { "InmailCmd",     kRegInmailCmd },
    { "OutmailCmd",    kRegOutmailCmd },     { "FwSts",         kRegFwSts },
};

class USB4Probe : public IOService {
    OSDeclareDefaultStructors(USB4Probe)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
private:
    static void putNum(OSDictionary *d, const char *key, uint64_t v, unsigned bits);
    void readNHI(IOPCIDevice *pci);
    void readRootPorts();
    static OSDictionary *describeRootPort(IOPCIDevice *rp);
};

OSDefineMetaClassAndStructors(USB4Probe, IOService)

void USB4Probe::putNum(OSDictionary *d, const char *key, uint64_t v, unsigned bits)
{
    OSNumber *n = OSNumber::withNumber(v, bits);
    if (n) { d->setObject(key, n); n->release(); }
}

IOService *USB4Probe::probe(IOService *provider, SInt32 *score)
{
    int flag = 0;
    if (PE_parse_boot_argn("-usb4off", &flag, sizeof flag)) return nullptr;
    if (!PE_parse_boot_argn("-usb4probe", &flag, sizeof flag)) return nullptr;
    return IOService::probe(provider, score);
}

/* The NHI's config header, power state, then the MMIO allowlist if the BAR decodes and the function is in D0. */
void USB4Probe::readNHI(IOPCIDevice *pci)
{
    const uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
    setProperty("NHICommand", cmd, 16);
    setProperty("NHIClassCode", pci->configRead32(kIOPCIConfigRevisionID) >> 8, 32);
    setProperty("NHIBAR0", pci->configRead32(kIOPCIConfigBaseAddress0), 32);
    setProperty("NHIBAR0Hi", pci->configRead32(kIOPCIConfigBaseAddress1), 32);

    UInt8 off = 0;
    unsigned dstate = 0;
    if (pci->findPCICapability(kIOPCICapabilityIDPowerManagement, &off) && off) {
        const uint16_t pmcsr = pci->configRead16(off + 4);
        dstate = pmcsr & 0x3;
        setProperty("NHIPMCSR", pmcsr, 16);
    }
    off = 0;
    if (pci->findPCICapability(kIOPCICapabilityIDMSIX, &off) && off)
        setProperty("NHIMSIXCount", (pci->configRead16(off + 2) & 0x7FF) + 1, 16);

    if (!(cmd & kIOPCICommandMemorySpace)) {
        setProperty("NHIStatus", "memory decode disabled; MMIO not read");
        U4_LOG("memory decode off (cmd %04x); MMIO not read", cmd);
        return;
    }
    if (dstate != 0) {
        /* D1 phase never changes power state, and MMIO is undefined outside D0. */
        setProperty("NHIStatus", "not in D0; MMIO not read");
        U4_LOG("NHI in D%u; MMIO not read", dstate);
        return;
    }

    IODeviceMemory *bar = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar || bar->getLength() < kMinBarSize) {
        setProperty("NHIStatus", "BAR0 missing or smaller than 256 KB; MMIO not read");
        return;
    }
    IOMemoryMap *map = bar->createMappingInTask(kernel_task, 0,
                                                kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly, 0,
                                                kMinBarSize);
    if (!map) {
        setProperty("NHIStatus", "could not map BAR0");
        return;
    }
    volatile uint8_t *base = (volatile uint8_t *)map->getVirtualAddress();
    setProperty("NHIBAR0Phys", map->getPhysicalAddress(), 64);
    setProperty("NHIBAR0Size", bar->getLength(), 64);

    OSDictionary *regs = OSDictionary::withCapacity(sizeof kAllowlist / sizeof kAllowlist[0]);
    uint32_t caps = 0, fwsts = 0;
    for (const auto &r : kAllowlist) {
        const uint32_t v = *(volatile uint32_t *)(base + r.off);
        if (r.off == kRegCaps) caps = v;
        if (r.off == kRegFwSts) fwsts = v;
        if (regs) putNum(regs, r.name, v, 32);
    }
    if (regs) { setProperty("NHIRegisters", regs); regs->release(); }
    map->release();   /* nothing stays mapped */

    if (caps == 0xFFFFFFFFu) {
        setProperty("NHIStatus", "MMIO reads all-ones; function not responding");
        U4_LOG("MMIO all-ones");
        return;
    }
    setProperty("NHIHopCount", caps & 0x3FF, 16);
    setProperty("NHIVersion", (caps >> 16) & 0xFF, 8);
    setProperty("NHIFirmwareCM", (fwsts & 0x1) ? "running (ICM_EN)" : "not running");
    setProperty("NHINVMAuthDone", (fwsts >> 31) & 1, 8);
    setProperty("NHIStatus", "read");
    U4_LOG("caps %08x (hops %u, version %02x) fw_sts %08x: ICM %s, NVM auth %s", caps, caps & 0x3FF,
           (caps >> 16) & 0xFF, fwsts, (fwsts & 1) ? "running" : "off", (fwsts >> 31) ? "done" : "not done");
}

/* Link, slot and bus window of one TBT root port, plus every PCI function macOS enumerated behind it. */
OSDictionary *USB4Probe::describeRootPort(IOPCIDevice *rp)
{
    OSDictionary *d = OSDictionary::withCapacity(12);
    if (!d) return nullptr;
    putNum(d, "device-id", rp->configRead16(kIOPCIConfigDeviceID), 16);
    putNum(d, "location", ((uint32_t)rp->getBusNumber() << 16) | ((uint32_t)rp->getDeviceNumber() << 8) |
                          rp->getFunctionNumber(), 32);
    putNum(d, "command", rp->configRead16(kIOPCIConfigCommand), 16);
    const uint32_t buses = rp->configRead32(0x18);
    const unsigned sec = (buses >> 8) & 0xFF, sub = (buses >> 16) & 0xFF;
    putNum(d, "secondary-bus", sec, 8);
    putNum(d, "subordinate-bus", sub, 8);
    putNum(d, "mem-window", rp->configRead32(0x20), 32);
    putNum(d, "pref-window", rp->configRead32(0x24), 32);

    UInt8 off = 0;
    if (rp->findPCICapability(kIOPCICapabilityIDPCIExpress, &off) && off) {
        const uint16_t lnksta = rp->configRead16(off + 0x12);
        putNum(d, "link-status", lnksta, 16);           /* bit 13 DLL active, 9:4 width, 3:0 speed */
        putNum(d, "slot-capabilities", rp->configRead32(off + 0x14), 32);   /* bit 6 hot-plug capable */
        putNum(d, "slot-status", rp->configRead16(off + 0x1A), 16);        /* bit 6 presence, bit 8 DLL changed */
        U4_LOG("root port %04x: buses %u-%u link %04x (DLL %s, x%u gen%u)", rp->configRead16(kIOPCIConfigDeviceID),
               sec, sub, lnksta, (lnksta & 0x2000) ? "up" : "down", (lnksta >> 4) & 0x3F, lnksta & 0xF);
    }

    /* Whatever IOPCIFamily enumerated in this root port's bus range: the dock's 15EF/15F0 if a tunnel was up. */
    OSArray *below = OSArray::withCapacity(4);
    OSIterator *it = sec ? IOService::getMatchingServices(IOService::serviceMatching("IOPCIDevice")) : nullptr;
    if (it && below) {
        while (OSObject *o = it->getNextObject()) {
            IOPCIDevice *p = OSDynamicCast(IOPCIDevice, o);
            if (!p || below->getCount() >= kMaxBelow) continue;
            const unsigned b = p->getBusNumber();
            if (b < sec || b > sub) continue;
            OSNumber *n = OSNumber::withNumber(((uint64_t)b << 40) | ((uint64_t)p->getDeviceNumber() << 35) |
                                               ((uint64_t)p->getFunctionNumber() << 32) |
                                               p->configRead32(kIOPCIConfigVendorID), 64);  /* bus.dev.fn | id */
            if (n) { below->setObject(n); n->release(); }
        }
    }
    if (it) it->release();
    if (below) { d->setObject("enumerated-below", below); below->release(); }
    return d;
}

void USB4Probe::readRootPorts()
{
    OSArray *ports = OSArray::withCapacity(4);
    OSIterator *it = IOService::getMatchingServices(IOService::serviceMatching("IOPCIDevice"));
    if (it && ports) {
        while (OSObject *o = it->getNextObject()) {
            IOPCIDevice *p = OSDynamicCast(IOPCIDevice, o);
            if (!p || p->configRead16(kIOPCIConfigVendorID) != kVendorIntel) continue;
            const uint16_t id = p->configRead16(kIOPCIConfigDeviceID);
            if (id < kRootPortFirst || id > kRootPortLast) continue;
            OSDictionary *d = describeRootPort(p);
            if (d) { ports->setObject(d); d->release(); }
        }
    }
    if (it) it->release();
    if (ports) {
        setProperty("TBTRootPortCount", ports->getCount(), 8);
        setProperty("TBTRootPorts", ports);
        ports->release();
    }
}

bool USB4Probe::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;

    const uint16_t ven = pci->configRead16(kIOPCIConfigVendorID), dev = pci->configRead16(kIOPCIConfigDeviceID);
    if (ven != kVendorIntel || dev != kNHI7EC2) {
        setProperty("USB4ProbeStatus", "unexpected device id; nothing read");
        return true;
    }
    readNHI(pci);
    readRootPorts();
    setProperty("USB4ProbeStatus", "done");
    return true;
}

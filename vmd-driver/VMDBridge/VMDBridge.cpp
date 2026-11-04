/*
 * VMDBridge - phase 1 of a macOS driver for Intel VMD 8086:AD0B (Arrow Lake), Alienware AAT2250.
 *
 * VMDController matches the VMD's IOPCIDevice, maps its CFGBAR (ECAM of the VMD domain) and hands a
 * second PCI root to IOPCIFamily: VMDHostBridge (an IOPCIHostBridge) attached to VMDDomainNub, a plain
 * IOService, because IOPCIConfigurator::addHostBridge panics when a host bridge's provider is an IOPCIDevice.
 *
 * SAFETY POLICY lives in the config accessors and is not optional:
 *  - only type-0 functions on the VMD root bus with class 0x010601 (SATA AHCI) are visible;
 *    every other function, and every bridge, reads as "no device" and ignores writes. The Windows
 *    NVMe (behind a remapped root port) is therefore never enumerated, configured or mounted.
 *  - MSI / MSI-X capabilities are removed from the visible capability chain and the interrupt pin
 *    reads 0, so IOPCIFamily never programs an interrupt that VMD remapping would misroute
 *    (0xAD0B cannot bypass MSI remapping: FreeBSD vmd.c, Linux vmd.c).
 *  - level 1 (-vmdbridge or vmdbridge=1) also disguises the class as 0x018000 so no storage driver
 *    attaches: it proves enumeration and BAR placement only. Level 2 (vmdbridge=2) shows the real
 *    class and is meant for the phase-2 interrupt path; without it AppleAHCIPort has no interrupt.
 *  - -vmdoff disables everything; nothing loads without an explicit vmdbridge boot-arg.
 */
#include "vmd_sdk_shim.h"   /* must precede IOKit headers */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include "IOPCIHostBridgeMirror.h"
#include "VMDInterrupts.h"

#define VMD_LOG(fmt, ...) IOLog("VMDBridge: " fmt "\n", ##__VA_ARGS__)

enum {
    kVMDRegVMCAP        = 0x40,
    kVMDRegVMCONFIG     = 0x44,
    kVMDMaxAllowed      = 8,
    kVMDMaxHiddenCaps   = 4,
    kVMDClassAHCI       = 0x010601,
    kVMDClassDisguised  = 0x018000,   /* "other mass storage": nothing in macOS matches it */
    kVMDDomainId        = 0x100,      /* distinct from the ACPI roots (domain 0) */
};

struct VMDFunction {
    uint8_t  dev, fn;
    uint8_t  firstVisibleCap;          /* virtual value of config 0x34 */
    uint8_t  hiddenCount;
    uint8_t  msiOff;                   /* MSI capability (0x05) offset, programmed by phase 2; 0 if none */
    uint8_t  hiddenOff[kVMDMaxHiddenCaps];
    uint8_t  hiddenLen[kVMDMaxHiddenCaps];
    uint8_t  nextVisible[256 / 4];     /* per dword-aligned visible cap header: virtual next pointer */
};

enum { kVMDPowerStateCount = 2 };
static IOPMPowerState gVMDPowerStates[kVMDPowerStateCount] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static int vmdLevel()
{
    int flag = 0, level = 0;
    if (PE_parse_boot_argn("-vmdoff", &flag, sizeof flag)) return 0;
    if (PE_parse_boot_argn("vmdbridge", &level, sizeof level)) return level == 2 ? 2 : (level == 1 ? 1 : 0);
    if (PE_parse_boot_argn("-vmdbridge", &flag, sizeof flag)) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------------------------- */

class VMDDomainNub : public IOService {
    OSDeclareDefaultStructors(VMDDomainNub)
};
OSDefineMetaClassAndStructors(VMDDomainNub, IOService)

/* ---------------------------------------------------------------------------------------------- */

class VMDHostBridge : public IOPCIHostBridge {
    OSDeclareDefaultStructors(VMDHostBridge)
public:
    bool setDomain(volatile uint8_t *ecam, uint8_t busStart, uint64_t window, uint64_t windowLen,
                   const VMDFunction *allowed, unsigned nAllowed, int level, VMDInterruptController *ic);
    bool configure(IOService *provider) override;
    bool publishNub(IOPCIDevice *nub, UInt32 index) override;
    IOReturn restoreDeviceState(IOPCIDevice *device, IOOptionBits options) override;
    UInt8 firstBusNum(void) override { return fBus; }
    UInt8 lastBusNum(void) override { return fBus; }       /* bridges are never exposed: one bus */
    IOPCIAddressSpace getBridgeSpace(void) override;
    IODeviceMemory *ioDeviceMemory(void) override { return nullptr; }   /* no I/O port window: AHCI uses its MMIO ABAR */
    UInt32 configRead32(IOPCIAddressSpace space, UInt8 offset) override;
    void configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data) override;
    UInt16 configRead16(IOPCIAddressSpace space, UInt8 offset) override;
    void configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data) override;
    UInt8 configRead8(IOPCIAddressSpace space, UInt8 offset) override;
    void configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data) override;
private:
    const VMDFunction *lookup(IOPCIAddressSpace space) const;
    volatile uint8_t *addr(IOPCIAddressSpace space, unsigned reg) const;
    uint32_t virtualRead32(const VMDFunction *f, IOPCIAddressSpace space, unsigned reg) const;
    bool writeAllowed(const VMDFunction *f, unsigned reg, unsigned len) const;

    volatile uint8_t *fECAM = nullptr;
    uint8_t   fBus = 0;
    uint64_t  fMem1 = 0, fMem1Len = 0;
    int       fLevel = 0;
    unsigned  fCount = 0;
    VMDFunction fAllowed[kVMDMaxAllowed];
    IOSimpleLock *fLock = nullptr;
    VMDInterruptController *fIC = nullptr;   /* level 2 only; owned by VMDController */
};
OSDefineMetaClassAndStructors(VMDHostBridge, IOPCIHostBridge)

bool VMDHostBridge::setDomain(volatile uint8_t *ecam, uint8_t busStart, uint64_t mem1, uint64_t mem1Len,
                              const VMDFunction *allowed, unsigned nAllowed, int level,
                              VMDInterruptController *ic)
{
    fLock = IOSimpleLockAlloc();
    if (!fLock || nAllowed > kVMDMaxAllowed) return false;
    fECAM = ecam; fBus = busStart; fMem1 = mem1; fMem1Len = mem1Len; fLevel = level; fCount = nAllowed;
    for (unsigned i = 0; i < nAllowed; i++) fAllowed[i] = allowed[i];
    fIC = level == 2 ? ic : nullptr;
    return true;
}

/* IOPCIFamily restores what it can see; the MSI capability is hidden from it, so put it back afterwards. */
IOReturn VMDHostBridge::restoreDeviceState(IOPCIDevice *device, IOOptionBits options)
{
    const IOReturn ret = IOPCIBridge::restoreDeviceState(device, options);
    if (fIC && device) fIC->reprogramChild(device);
    return ret;
}

/* Level 2: route the child's interrupt through the VMD before any driver can match the nub. */
bool VMDHostBridge::publishNub(IOPCIDevice *nub, UInt32 index)
{
    if (nub && fIC) {
        IOPCIAddressSpace space;
        space.bits = 0;
        space.s.busNum = nub->getBusNumber();
        space.s.deviceNum = nub->getDeviceNumber();
        space.s.functionNum = nub->getFunctionNumber();
        const VMDFunction *f = lookup(space);
        if (!f || !f->msiOff || !fIC->attachChild(nub, f->dev, f->fn, f->msiOff))
            VMD_LOG("%02x.%u: no VMD interrupt route (MSI cap 0x%02x); driver will get no interrupt",
                    space.s.deviceNum, space.s.functionNum, f ? f->msiOff : 0);
    }
    return IOPCIBridge::publishNub(nub, index);
}

bool VMDHostBridge::configure(IOService *provider)
{
    if (!IOPCIHostBridge::configure(provider)) return false;
    /* Only the AHCI ABAR's own firmware-assigned range is advertised (review finding 1): the configurator does not
     * know about the hidden root port's / NVMe's MMIO inside MEMBAR1, so a wider window could let it relocate the
     * ABAR on top of them. With exactly one range that already holds the BAR, there is nothing to move it to. */
    if (!addBridgeMemoryRange((IOPhysicalAddress)fMem1, (IOPhysicalLength)fMem1Len, true)) {
        VMD_LOG("addBridgeMemoryRange(0x%llx, 0x%llx) refused", fMem1, fMem1Len);
        return false;
    }
    return true;
}

IOPCIAddressSpace VMDHostBridge::getBridgeSpace(void)
{
    IOPCIAddressSpace space;
    space.bits = 0;
    space.s.busNum = fBus;
    return space;
}

const VMDFunction *VMDHostBridge::lookup(IOPCIAddressSpace space) const
{
    if (space.s.busNum != fBus) return nullptr;
    for (unsigned i = 0; i < fCount; i++)
        if (fAllowed[i].dev == space.s.deviceNum && fAllowed[i].fn == space.s.functionNum) return &fAllowed[i];
    return nullptr;
}

volatile uint8_t *VMDHostBridge::addr(IOPCIAddressSpace space, unsigned reg) const
{
    /* ECAM index is bus - busStart, and only the root bus is ever visible, so it is always 0. */
    return fECAM + ((uint64_t)space.s.deviceNum << 15) + ((uint64_t)space.s.functionNum << 12) + reg;
}

uint32_t VMDHostBridge::virtualRead32(const VMDFunction *f, IOPCIAddressSpace space, unsigned reg) const
{
    uint32_t v = *(volatile uint32_t *)addr(space, reg);
    switch (reg) {
        case 0x08:                                           /* class code in bits 31:8 */
            if (fLevel < 2) v = (v & 0xFF) | ((uint32_t)kVMDClassDisguised << 8);
            break;
        case 0x34:
            v = (v & ~0xFFu) | f->firstVisibleCap;
            break;
        case 0x3C:                                           /* interrupt pin (bits 15:8) reads 0 */
            v &= ~0xFF00u;
            break;
        default:
            if (reg >= 0x40 && reg < 0x100 && f->nextVisible[reg / 4])
                v = (v & ~0xFF00u) | ((uint32_t)(f->nextVisible[reg / 4] == 0xFF ? 0 : f->nextVisible[reg / 4]) << 8);
            break;
    }
    return v;
}

bool VMDHostBridge::writeAllowed(const VMDFunction *f, unsigned reg, unsigned len) const
{
    for (unsigned i = 0; i < f->hiddenCount; i++)
        if (reg + len > f->hiddenOff[i] && reg < (unsigned)f->hiddenOff[i] + f->hiddenLen[i]) return false;
    return !(reg <= 0x3D && reg + len > 0x3D);              /* never let anyone set the pin */
}

UInt32 VMDHostBridge::configRead32(IOPCIAddressSpace space, UInt8 offset)
{
    const unsigned reg = ((unsigned)space.es.registerNumExtended << 8) | (offset & 0xFCu);
    const VMDFunction *f = lookup(space);
    if (!f || reg >= 0x1000) return 0xFFFFFFFFu;
    IOSimpleLockLock(fLock);
    const uint32_t v = virtualRead32(f, space, reg);
    IOSimpleLockUnlock(fLock);
    return v;
}

UInt16 VMDHostBridge::configRead16(IOPCIAddressSpace space, UInt8 offset)
{
    return (UInt16)(configRead32(space, offset) >> ((offset & 2) * 8));
}

UInt8 VMDHostBridge::configRead8(IOPCIAddressSpace space, UInt8 offset)
{
    return (UInt8)(configRead32(space, offset) >> ((offset & 3) * 8));
}

void VMDHostBridge::configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data)
{
    const unsigned reg = ((unsigned)space.es.registerNumExtended << 8) | (offset & 0xFCu);
    const VMDFunction *f = lookup(space);
    if (!f || reg >= 0x1000 || !writeAllowed(f, reg, 4)) return;
    IOSimpleLockLock(fLock);
    *(volatile uint32_t *)addr(space, reg) = data;
    IOSimpleLockUnlock(fLock);
}

void VMDHostBridge::configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data)
{
    const unsigned reg = ((unsigned)space.es.registerNumExtended << 8) | (offset & 0xFEu);
    const VMDFunction *f = lookup(space);
    if (!f || reg >= 0x1000 || !writeAllowed(f, reg, 2)) return;
    IOSimpleLockLock(fLock);
    *(volatile uint16_t *)addr(space, reg) = data;
    IOSimpleLockUnlock(fLock);
}

void VMDHostBridge::configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data)
{
    const unsigned reg = ((unsigned)space.es.registerNumExtended << 8) | offset;
    const VMDFunction *f = lookup(space);
    if (!f || reg >= 0x1000 || !writeAllowed(f, reg, 1)) return;
    IOSimpleLockLock(fLock);
    *(volatile uint8_t *)addr(space, reg) = data;
    IOSimpleLockUnlock(fLock);
}

/* ---------------------------------------------------------------------------------------------- */

/* No stop()/free() teardown by design (review finding 6): IOPCIConfigurator keeps a raw pointer to the host
 * bridge for the life of the system, so the bridge, its nub and the CFGBAR mapping must never be released once
 * started. Unloading this kext after a successful start is not supported; use -vmdoff and reboot. */
class VMDController : public IOService {
    OSDeclareDefaultStructors(VMDController)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
private:
    static bool abiMatches();
    unsigned findAllowed(volatile uint8_t *ecam, VMDFunction *out);
    static void buildCapView(volatile uint8_t *fn, VMDFunction *f);
    static bool sizeABAR(volatile uint8_t *fn, uint64_t *base, uint64_t *len);

    IOMemoryMap   *fCfgMap = nullptr;
    VMDDomainNub  *fNub = nullptr;
    VMDHostBridge *fBridge = nullptr;
    VMDInterruptController *fIC = nullptr;
    bool           fPMStarted = false;
};
OSDefineMetaClassAndStructors(VMDController, IOService)

IOService *VMDController::probe(IOService *provider, SInt32 *score)
{
    if (!vmdLevel()) return nullptr;
    return IOService::probe(provider, score);
}

bool VMDController::abiMatches()
{
    /* Our subclass is laid out against the 726.100.6 declaration; a different kernel layout would corrupt memory. */
    const unsigned expected = (unsigned)sizeof(IOPCIHostBridge);
    const unsigned actual = (unsigned)IOPCIHostBridge::metaClass->getClassSize();
    if (expected != actual) VMD_LOG("IOPCIHostBridge size %u, built for %u: refusing to load", actual, expected);
    return expected == actual;
}

/*
 * The visible capability chain skips MSI (0x05) and MSI-X (0x11). nextVisible[] holds, for each visible
 * header, the next visible offset (0xFF encodes "end of list" so that 0 can mean "not a header").
 */
void VMDController::buildCapView(volatile uint8_t *fn, VMDFunction *f)
{
    uint8_t visible[48]; unsigned nv = 0;
    unsigned ptr = (*(volatile uint32_t *)(fn + 0x34)) & 0xFC;
    for (unsigned n = 0; ptr >= 0x40 && ptr < 0x100 && n < 48; n++) {
        const uint32_t hdr = *(volatile uint32_t *)(fn + ptr);
        const uint8_t id = hdr & 0xFF;
        if ((id == 0x05 || id == 0x11) && f->hiddenCount < kVMDMaxHiddenCaps) {
            f->hiddenOff[f->hiddenCount] = (uint8_t)ptr;
            if (id == 0x05) {                                    /* MSI: 10 + 4 if 64-bit + 10 if per-vector mask */
                const uint16_t ctl = (uint16_t)(hdr >> 16);
                f->hiddenLen[f->hiddenCount] = (uint8_t)(10 + ((ctl & 0x80) ? 4 : 0) + ((ctl & 0x100) ? 10 : 0));
            } else {
                f->hiddenLen[f->hiddenCount] = 12;               /* MSI-X */
            }
            if (id == 0x05) f->msiOff = (uint8_t)ptr;
            f->hiddenCount++;
        } else {
            visible[nv++] = (uint8_t)ptr;
        }
        ptr = (hdr >> 8) & 0xFC;
    }
    f->firstVisibleCap = nv ? visible[0] : 0;
    for (unsigned i = 0; i < nv; i++) f->nextVisible[visible[i] / 4] = i + 1 < nv ? visible[i + 1] : 0xFF;
}

/*
 * AHCI ABAR (BAR5, 32-bit memory) base and size. Sizing writes all-ones with memory decode off and restores both,
 * the standard PCI procedure; it runs before any macOS driver can bind, on the allowed AHCI function only.
 */
bool VMDController::sizeABAR(volatile uint8_t *fn, uint64_t *base, uint64_t *len)
{
    volatile uint16_t *cmd = (volatile uint16_t *)(fn + 0x04);
    volatile uint32_t *bar = (volatile uint32_t *)(fn + 0x24);
    /* Review (phase 2): any other memory BAR could not be placed inside the ABAR-only window. Refuse it. */
    for (unsigned i = 0; i < 5; i++) {
        const uint32_t b = *(volatile uint32_t *)(fn + 0x10 + 4 * i);
        if (b && !(b & 0x1)) return false;
    }
    const uint32_t orig = *bar;
    if ((orig & 0x1) || (orig & 0x6) != 0) return false;           /* must be 32-bit memory */
    const uint16_t c = *cmd;
    *cmd = (uint16_t)(c & ~0x0002u);
    *bar = 0xFFFFFFFFu;
    const uint32_t mask = *bar & ~0xFu;
    *bar = orig;
    *cmd = c;
    if (!mask || *bar != orig) return false;
    *base = orig & ~0xFull;
    *len = (uint64_t)(~mask) + 1;
    return *base != 0;
}

/* Root-bus, type-0, SATA AHCI functions only. Bridges (and so the NVMe behind its root port) never qualify. */
unsigned VMDController::findAllowed(volatile uint8_t *ecam, VMDFunction *out)
{
    unsigned n = 0;
    for (unsigned d = 0; d < 32; d++)
        for (unsigned f = 0; f < 8 && n < kVMDMaxAllowed; f++) {
            volatile uint8_t *fn = ecam + ((uint64_t)d << 15) + ((uint64_t)f << 12);
            const uint32_t id = *(volatile uint32_t *)fn;
            if (id == 0xFFFFFFFFu || id == 0) { if (f == 0) break; continue; }
            const uint32_t hdr = (*(volatile uint32_t *)(fn + 0x0C) >> 16) & 0xFF;
            const uint32_t cls = *(volatile uint32_t *)(fn + 0x08) >> 8;
            VMD_LOG("root %02x.%u %04x:%04x class %06x hdr %02x", d, f, id & 0xFFFF, id >> 16, cls, hdr);
            if ((hdr & 0x7F) == 0 && cls == kVMDClassAHCI) {
                VMDFunction *v = &out[n++];
                bzero(v, sizeof *v);
                v->dev = (uint8_t)d; v->fn = (uint8_t)f;
                buildCapView(fn, v);
            }
            if (f == 0 && !(hdr & 0x80)) break;
        }
    return n;
}

bool VMDController::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    const int level = vmdLevel();
    if (!pci || !level) return false;
    if (pci->configRead16(kIOPCIConfigVendorID) != 0x8086 || pci->configRead16(kIOPCIConfigDeviceID) != 0xAD0B)
        return false;
    if (!abiMatches()) { pci->setProperty("VMDBridgeStatus", "IOPCIHostBridge ABI mismatch"); return false; }

    const uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
    if (!(cmd & kIOPCICommandMemorySpace)) { setProperty("VMDStatus", "memory decode off"); return false; }

    /* v0.1.1: make the hardware decode where IOPCIFamily allocated. Boot 12: IOPCIFamily sized the 64-bit CFGBAR
     * (BAR0/1) and MEMBAR2 (BAR4/5) as 32-bit BARs, wrote only their low dwords and left the firmware's high dwords
     * (0xB8), so the VMD decoded 0xB8_8000_0000 / 0xB8_6500_0000 while macOS reserved 0x8000_0000 / 0x6500_0000.
     * Clear a high dword only when IOPCIFamily's own range for that BAR is below 4 GB and equals its low dword, with
     * memory decode off during the write (standard BAR programming). Nothing else in macOS uses the VMD. */
    {
        static const uint8_t lo[2] = { kIOPCIConfigBaseAddress0, kIOPCIConfigBaseAddress4 };
        static const char *const before[2] = { "VMDBAR1Before", "VMDBAR5Before" };
        bool fixed = false;
        for (unsigned i = 0; i < 2; i++) {
            const uint32_t l = pci->configRead32(lo[i]), h = pci->configRead32(lo[i] + 4);
            IODeviceMemory *m = pci->getDeviceMemoryWithRegister(lo[i]);
            pci->setProperty(before[i], h, 32);
            if ((l & 0x7) != 0x4 || !h || !m) continue;
            const uint64_t os = m->getPhysicalAddress();
            if (os >> 32 || os != (l & ~0xFull)) continue;
            pci->configWrite16(kIOPCIConfigCommand, (uint16_t)(cmd & ~kIOPCICommandMemorySpace));
            pci->configWrite32(lo[i] + 4, 0);
            pci->configWrite16(kIOPCIConfigCommand, cmd);
            fixed = true;
        }
        pci->setProperty("VMDBAR1After", pci->configRead32(kIOPCIConfigBaseAddress1), 32);
        pci->setProperty("VMDBAR5After", pci->configRead32(kIOPCIConfigBaseAddress5), 32);
        pci->setProperty("VMDBARHighCleared", fixed);
        VMD_LOG("high dwords %s", fixed ? "cleared to match IOPCIFamily" : "left alone");
    }

    const uint16_t vmcap = pci->configRead16(kVMDRegVMCAP), vmcfg = pci->configRead16(kVMDRegVMCONFIG);
    uint8_t busStart = 0;
    if (vmcap & 1) busStart = ((vmcfg >> 8) & 3) == 0 ? 0 : (((vmcfg >> 8) & 3) == 1 ? 128 : 224);

    IODeviceMemory *cfg = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IODeviceMemory *mem1 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!cfg || !mem1) { setProperty("VMDStatus", "missing CFGBAR or MEMBAR1"); return false; }
    fCfgMap = cfg->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache);
    if (!fCfgMap) { setProperty("VMDStatus", "could not map CFGBAR"); return false; }
    if (fCfgMap->getLength() < (1ull << 20)) {                        /* review finding 2: bus 0 needs 1 MB */
        setProperty("VMDStatus", "CFGBAR mapping shorter than one bus");
        goto fail;
    }
    {
    volatile uint8_t *ecam = (volatile uint8_t *)fCfgMap->getVirtualAddress();

    VMDFunction allowed[kVMDMaxAllowed];
    const unsigned n = findAllowed(ecam, allowed);
    uint64_t abar = 0, abarLen = 0;
    setProperty("VMDLevel", (unsigned long long)level, 32);
    setProperty("VMDBusStart", busStart, 8);
    setProperty("VMDAllowedFunctions", n, 32);
    if (!n) { setProperty("VMDStatus", "no AHCI function on the VMD root bus"); goto fail; }
    if (n != 1) { setProperty("VMDStatus", "more than one AHCI function: phase 1 handles exactly one"); goto fail; }
    if (!sizeABAR(ecam + ((uint64_t)allowed[0].dev << 15) + ((uint64_t)allowed[0].fn << 12), &abar, &abarLen) ||
        abar < mem1->getPhysicalAddress() || abar + abarLen > mem1->getPhysicalAddress() + mem1->getLength()) {
        setProperty("VMDStatus", "AHCI ABAR missing or outside MEMBAR1");
        goto fail;
    }
    setProperty("VMDABARBase", abar, 64);
    setProperty("VMDABARLength", abarLen, 64);

    /* Review finding 5: controller and nub join the power tree so IOPCIBridge::start's joinPMtree has a parent. */
    PMinit();
    fPMStarted = true;
    pci->joinPMtree(this);
    registerPowerDriver(this, gVMDPowerStates, kVMDPowerStateCount);

    fNub = OSTypeAlloc(VMDDomainNub);
    if (!fNub || !fNub->init((OSDictionary *)nullptr)) goto fail;
    {
        const uint32_t dom = kVMDDomainId;
        OSData *d = OSData::withBytes(&dom, sizeof dom);
        if (d) { fNub->setProperty("domain-id", d); d->release(); }
        fNub->setName("vmd-domain");
    }
    if (!fNub->attach(this)) goto fail;
    fNub->PMinit();
    joinPMtree(fNub);
    fNub->registerPowerDriver(fNub, gVMDPowerStates, kVMDPowerStateCount);

    if (level == 2) {
        fIC = OSTypeAlloc(VMDInterruptController);
        if (!fIC || !fIC->initWithDomain(pci, ecam) || !fIC->startVectors()) {
            setProperty("VMDStatus", "level 2: VMD interrupt setup failed");
            goto fail;
        }
    }

    fBridge = OSTypeAlloc(VMDHostBridge);
    if (!fBridge || !fBridge->init((OSDictionary *)nullptr) ||
        !fBridge->setDomain(ecam, busStart, abar, abarLen, allowed, n, level, fIC))
        goto fail;
    {
        SInt32 score = 0;
        if (!fBridge->attach(fNub)) goto fail;
        if (!fBridge->probe(fNub, &score) || !fBridge->start(fNub)) {
            fBridge->detach(fNub);
            goto fail;
        }
    }
    setProperty("VMDStatus", level == 1 ? "bridge up (level 1: class disguised)" : "bridge up (level 2)");
    VMD_LOG("bridge started: bus %u, %u AHCI function(s), ABAR 0x%llx+0x%llx, level %d", busStart, n,
            abar, abarLen, level);
    return true;
    }

fail:
    if (!getProperty("VMDStatus")) setProperty("VMDStatus", "start failed");
    /* This object is freed after a failed start; copy the reason onto the VMD's own node so ioreg still shows it. */
    if (pci) {
        if (OSObject *s = getProperty("VMDStatus")) pci->setProperty("VMDBridgeStatus", s);
        if (OSObject *n = getProperty("VMDAllowedFunctions")) pci->setProperty("VMDBridgeAllowedFunctions", n);
    }
    OSSafeReleaseNULL(fBridge);
    if (fIC) { fIC->stopVectors(); OSSafeReleaseNULL(fIC); }
    if (fNub) { fNub->PMstop(); fNub->detach(this); OSSafeReleaseNULL(fNub); }
    if (fPMStarted) { PMstop(); fPMStarted = false; }
    OSSafeReleaseNULL(fCfgMap);
    return false;
}

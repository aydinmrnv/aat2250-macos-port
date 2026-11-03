#include "VMDInterrupts.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>

extern "C" boolean_t ml_at_interrupt_context(void);
extern "C" boolean_t ml_set_interrupts_enabled(boolean_t enable);

#define VMD_LOG(fmt, ...) IOLog("VMDBridge: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(VMDInterruptController, IOInterruptController)

bool VMDInterruptController::initWithDomain(IOPCIDevice *vmd, volatile uint8_t *ecam)
{
    if (!IOService::init((OSDictionary *)nullptr)) return false;
    fLock = IOSimpleLockAlloc();
    if (!fLock || !vmd || !ecam) return false;
    fVMD = vmd; fECAM = ecam;
    bzero(fChild, sizeof fChild);
    char name[48];
    snprintf(name, sizeof name, "VMDInterruptController-%llx", vmd->getRegistryEntryID());
    fName = OSSymbol::withCString(name);
    return fName != nullptr;
}

void VMDInterruptController::free()
{
    OSSafeReleaseNULL(fName);
    if (fLock) { IOSimpleLockFree(fLock); fLock = nullptr; }
    IOService::free();   /* IOInterruptController::free would walk 'vectors', which this class never allocates */
}

/* The VMD's own MSI-X: entries 0 and 1, delivered by the platform like any PCI device's. */
bool VMDInterruptController::startVectors()
{
    UInt8 msix = 0;
    if (!fVMD->findPCICapability(kIOPCICapabilityIDMSIX, &msix) || !msix ||
        (unsigned)(fVMD->configRead16(msix + 2) & 0x7FF) + 1 < kVMDVectorsRequested) {
        VMD_LOG("VMD MSI-X table missing or smaller than %d entries", (int)kVMDVectorsRequested);
        return false;
    }
    if (fVMD->configureInterrupts(kIOInterruptTypePCIMessagedX, kVMDVectorsRequested, kVMDVectorsRequested)
        != kIOReturnSuccess) {
        VMD_LOG("VMD MSI-X configuration failed");
        return false;
    }
    int messaged[kVMDVectorsRequested * 2]; unsigned nm = 0;
    for (int idx = 0; idx < 64 && nm < kVMDVectorsRequested * 2; idx++) {
        int type = 0;
        if (fVMD->getInterruptType(idx, &type) != kIOReturnSuccess) break;
        if (type & kIOInterruptTypePCIMessaged) messaged[nm++] = idx;
    }
    /* Entry 1 must be among them; with fewer than two messaged vectors it cannot be (review: configureInterrupts
     * may succeed without reallocating if someone configured the VMD first). */
    if (nm < kVMDVectorsRequested) { VMD_LOG("only %u messaged VMD vector(s); need %d", nm, (int)kVMDVectorsRequested); return false; }
    for (unsigned i = 0; i < nm; i++) {
        /* Which IOKit index maps to which MSI-X entry is not documented, so every messaged vector dispatches. */
        if (fVMD->registerInterrupt(messaged[i], this, &VMDInterruptController::vmdVectorFired, nullptr)
            != kIOReturnSuccess) continue;
        fVMD->enableInterrupt(messaged[i]);
        fVecIndex[fVecCount++] = messaged[i];
    }
    if (fVecCount < kVMDVectorsRequested) { stopVectors(); return false; }
    if (getPlatform()->registerInterruptController(const_cast<OSSymbol *>(fName), this) != kIOReturnSuccess) {
        stopVectors();
        return false;
    }
    VMD_LOG("%u VMD vector(s) registered; children target VMD entry %d", fVecCount, (int)kChildVMDEntry);
    return true;
}

void VMDInterruptController::stopVectors()
{
    for (unsigned i = 0; i < fVecCount; i++) {
        fVMD->disableInterrupt(fVecIndex[i]);
        fVMD->unregisterInterrupt(fVecIndex[i]);
    }
    fVecCount = 0;
}

volatile uint8_t *VMDInterruptController::childBase(const Child &c) const
{
    return fECAM + ((uint64_t)c.dev << 15) + ((uint64_t)c.fn << 12);   /* children live on the root bus only */
}

/* MSI to VMD entry 1, one message, mask bits cleared, INTx off. */
void VMDInterruptController::programMSI(const Child &c)
{
    volatile uint8_t *base = childBase(c);
    const unsigned cap = c.msiCap;
    uint16_t ctrl = *(volatile uint16_t *)(base + cap + 2);
    const bool is64 = (ctrl & 0x80) != 0, maskable = (ctrl & 0x100) != 0;
    *(volatile uint16_t *)(base + cap + 2) = (uint16_t)(ctrl & ~0x0001u);        /* disable while reprogramming */
    *(volatile uint32_t *)(base + cap + 4) = 0xFEE00000u | ((uint32_t)kChildVMDEntry << 12);
    if (is64) {
        *(volatile uint32_t *)(base + cap + 8) = 0;
        *(volatile uint16_t *)(base + cap + 12) = 0;
        if (maskable) *(volatile uint32_t *)(base + cap + 16) = 0;
    } else {
        *(volatile uint16_t *)(base + cap + 8) = 0;
        if (maskable) *(volatile uint32_t *)(base + cap + 12) = 0;
    }
    ctrl = (uint16_t)((ctrl & ~0x0070u) | 0x0001u);
    *(volatile uint16_t *)(base + cap + 2) = ctrl;
    *(volatile uint16_t *)(base + 0x04) = (uint16_t)(*(volatile uint16_t *)(base + 0x04) | 0x0400u);
}

/* Give the nub our controller and program its MSI capability to target VMD entry 1. */
bool VMDInterruptController::attachChild(IOPCIDevice *nub, uint8_t dev, uint8_t fn, uint8_t msiCapOffset)
{
    if (!nub || !msiCapOffset || fCount >= kMaxChildren) return false;

    const OSObject *ctl[1] = { fName };
    OSArray *controllers = OSArray::withObjects(ctl, 1, 1);
    const uint32_t spec = fCount;
    OSData *specData = OSData::withBytes(&spec, sizeof spec);
    const OSObject *sp[1] = { specData };
    OSArray *specifiers = specData ? OSArray::withObjects(sp, 1, 1) : nullptr;
    if (!controllers || !specifiers) {
        OSSafeReleaseNULL(controllers); OSSafeReleaseNULL(specifiers); OSSafeReleaseNULL(specData);
        return false;
    }
    nub->setProperty(gIOInterruptControllersKey, controllers);
    nub->setProperty(gIOInterruptSpecifiersKey, specifiers);
    controllers->release(); specifiers->release(); specData->release();

    Child &c = fChild[fCount];
    bzero(&c, sizeof c);
    c.nub = nub; c.dev = dev; c.fn = fn; c.msiCap = msiCapOffset;
    programMSI(c);
    fCount++;
    VMD_LOG("child %02x.%u: MSI cap 0x%02x -> VMD entry %d, specifier %u", dev, fn, msiCapOffset,
            (int)kChildVMDEntry, spec);
    return true;
}

/* Called after IOPCIFamily restores a child's config space, which never includes the hidden MSI capability. */
void VMDInterruptController::reprogramChild(IOService *nub)
{
    Child *c = findChild(nub);
    if (c) programMSI(*c);
}

VMDInterruptController::Child *VMDInterruptController::findChild(IOService *nub)
{
    for (unsigned i = 0; i < fCount; i++)
        if (fChild[i].nub == nub) return &fChild[i];
    return nullptr;
}

/* Wait for handlers already running on other CPUs. Never waits from interrupt context (a handler disabling
 * itself would spin forever on its own active count). */
void VMDInterruptController::waitIdle(Child *c)
{
    if (ml_at_interrupt_context()) return;
    while (c->active) IODelay(1);
}

IOReturn VMDInterruptController::registerInterrupt(IOService *nub, int source, void *target,
                                                  IOInterruptHandler handler, void *refCon)
{
    Child *c = findChild(nub);
    if (!c || source != 0) return kIOReturnNoInterrupt;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    c->enabled = false; c->pending = false;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    waitIdle(c);
    is = IOSimpleLockLockDisableInterrupt(fLock);
    c->target = target; c->refCon = refCon; c->handler = handler;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    return kIOReturnSuccess;
}

IOReturn VMDInterruptController::unregisterInterrupt(IOService *nub, int source)
{
    Child *c = findChild(nub);
    if (!c || source != 0) return kIOReturnNoInterrupt;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    c->enabled = false; c->pending = false;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    waitIdle(c);                                         /* the driver may free its state when we return */
    is = IOSimpleLockLockDisableInterrupt(fLock);
    c->handler = nullptr; c->target = nullptr; c->refCon = nullptr;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    return kIOReturnSuccess;
}

IOReturn VMDInterruptController::getInterruptType(IOService *nub, int source, int *interruptType)
{
    if (!findChild(nub) || source != 0 || !interruptType) return kIOReturnNoInterrupt;
    *interruptType = kIOInterruptTypeEdge;               /* MSI semantics */
    return kIOReturnSuccess;
}

IOReturn VMDInterruptController::enableInterrupt(IOService *nub, int source)
{
    Child *c = findChild(nub);
    if (!c || source != 0) return kIOReturnNoInterrupt;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    if (!c->handler) { IOSimpleLockUnlockEnableInterrupt(fLock, is); return kIOReturnNoInterrupt; }
    c->enabled = true;
    const bool replay = c->pending;
    c->pending = false;
    IOInterruptHandler h = c->handler; void *t = c->target, *r = c->refCon;
    if (replay) OSIncrementAtomic(&c->active);
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    if (replay) {
        /* An edge was dropped while disabled; deliver it now, with interrupts off as a primary handler expects. */
        (void)*(volatile uint32_t *)childBase(*c);       /* ARL004 ordering read, as on the interrupt path */
        const boolean_t was = ml_set_interrupts_enabled(FALSE);
        h(t, r, nub, 0);
        ml_set_interrupts_enabled(was);
        OSDecrementAtomic(&c->active);
    }
    return kIOReturnSuccess;
}

IOReturn VMDInterruptController::disableInterrupt(IOService *nub, int source)
{
    Child *c = findChild(nub);
    if (!c || source != 0) return kIOReturnNoInterrupt;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    c->enabled = false;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    waitIdle(c);
    return kIOReturnSuccess;
}

IOReturn VMDInterruptController::causeInterrupt(IOService *, int)
{
    return kIOReturnUnsupported;
}

/* Primary interrupt context. Every child shares the VMD entry, so each enabled child's handler runs and must
 * check its own status register, as AHCI handlers do. */
void VMDInterruptController::vmdVectorFired(OSObject *owner, void *, IOService *, int)
{
    VMDInterruptController *self = OSDynamicCast(VMDInterruptController, owner);
    if (!self) return;
    IOSimpleLockLock(self->fLock);
    for (unsigned i = 0; i < self->fCount; i++) {
        Child &c = self->fChild[i];
        if (!c.handler) continue;
        if (!c.enabled) { c.pending = true; continue; }
        (void)*(volatile uint32_t *)self->childBase(c);  /* ARL004: read to the child orders its posted writes */
        IOInterruptHandler h = c.handler; void *t = c.target, *r = c.refCon; IOService *n = c.nub;
        OSIncrementAtomic(&c.active);
        IOSimpleLockUnlock(self->fLock);
        h(t, r, n, 0);
        IOSimpleLockLock(self->fLock);
        OSDecrementAtomic(&c.active);
    }
    IOSimpleLockUnlock(self->fLock);
}

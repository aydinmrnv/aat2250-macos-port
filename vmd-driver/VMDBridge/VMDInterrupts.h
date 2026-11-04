/*
 * VMDInterruptController - phase 2: child interrupts of the VMD domain.
 *
 * 0xAD0B cannot bypass MSI remapping (FreeBSD vmd.c, Linux vmd.c), so a child's MSI must target a VMD MSI-X
 * table entry: address 0xFEE00000 | (entry << 12), data 0 (FreeBSD vmd_map_msi). Entry 0 is reserved on client
 * VMDs (VECTOR_OFFSET / OFFSET_FIRST_VECTOR), so children use entry 1. The VMD then raises its own MSI-X vector,
 * which the platform delivers normally because the VMD itself is an ordinary PCI device on PC00.
 *
 * Children are given IOInterruptControllers/IOInterruptSpecifiers naming this controller, so a driver's
 * registerInterrupt() lands here. On a VMD vector we first read the child's config space (Intel erratum ARL004:
 * the VMD may signal before the child's posted DMA writes land; a read *to the child* orders them — Linux vmd.c),
 * then call the child's handler.
 *
 * Review fixes (REVIEW-phase2.md): an MSI that arrives while the child is disabled is remembered and replayed on
 * enable (MSI is never resent); disable/unregister wait for an in-flight handler on another CPU; the MSI
 * capability, invisible to IOPCIFamily, is re-programmed after every config restore (sleep/wake, D3).
 */
#ifndef VMD_INTERRUPTS_H
#define VMD_INTERRUPTS_H

#include "vmd_sdk_shim.h"
#include <IOKit/IOInterruptController.h>
#include <IOKit/pci/IOPCIDevice.h>

class VMDInterruptController : public IOInterruptController {
    OSDeclareDefaultStructors(VMDInterruptController)
public:
    enum { kMaxChildren = 8, kChildVMDEntry = 1, kVMDVectorsRequested = 2 };

    bool initWithDomain(IOPCIDevice *vmd, volatile uint8_t *ecam);
    bool startVectors();
    void stopVectors();
    bool attachChild(IOPCIDevice *nub, uint8_t slot, uint8_t dev, uint8_t fn, uint8_t msiCapOffset);
    void reprogramChild(IOService *nub);

    IOReturn registerInterrupt(IOService *nub, int source, void *target, IOInterruptHandler handler,
                               void *refCon) override;
    IOReturn unregisterInterrupt(IOService *nub, int source) override;
    IOReturn getInterruptType(IOService *nub, int source, int *interruptType) override;
    IOReturn enableInterrupt(IOService *nub, int source) override;
    IOReturn disableInterrupt(IOService *nub, int source) override;
    IOReturn causeInterrupt(IOService *nub, int source) override;
    void free() override;

private:
    struct Child {
        IOService          *nub;
        uint8_t             slot, dev, fn, msiCap;   /* slot = ECAM bus slot (v0.1.2: AHCI is on slot 1) */
        void               *target;
        IOInterruptHandler  handler;
        void               *refCon;
        volatile bool       enabled;
        volatile bool       pending;    /* MSI arrived while disabled: replay on enable */
        volatile SInt32     active;     /* handlers currently running for this child */
    };
    static void vmdVectorFired(OSObject *owner, void *refCon, IOService *nub, int source);
    Child *findChild(IOService *nub);
    volatile uint8_t *childBase(const Child &c) const;
    void programMSI(const Child &c);
    void waitIdle(Child *c);

    Child             fChild[kMaxChildren];
    unsigned          fCount = 0;
    IOPCIDevice      *fVMD = nullptr;
    volatile uint8_t *fECAM = nullptr;
    const OSSymbol   *fName = nullptr;
    int               fVecIndex[kVMDVectorsRequested * 2];
    unsigned          fVecCount = 0;
    IOSimpleLock     *fLock = nullptr;
};

#endif

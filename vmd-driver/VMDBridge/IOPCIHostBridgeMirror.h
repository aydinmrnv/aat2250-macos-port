/*
 * Mirror of Apple's IOPCIHostBridge declaration, which IOPCIFamily exports but does not ship in the SDK.
 * Source: apple-oss-distributions/IOPCIFamily tag IOPCIFamily-726.100.6 (commit 503b7da7, the IOPCIFamily of
 * macOS 26.5 per distribution-macOS tag macos-265), file IOKit/pci/IOPCIPrivate.h lines 857-879.
 * Apple Public Source License 2.0 applies to that declaration.
 *
 * The layout must match the running kernel's IOPCIFamily exactly. VMDController checks
 * IOPCIHostBridge::metaClass->getClassSize() against sizeof() below before creating anything.
 */
#ifndef VMD_IOPCIHOSTBRIDGE_MIRROR_H
#define VMD_IOPCIHOSTBRIDGE_MIRROR_H

#include <IOKit/pci/IOPCIBridge.h>

class IOPCIHostBridgeData;

class IOPCIHostBridge : public IOPCIBridge
{
    OSDeclareDefaultStructors(IOPCIHostBridge);
public:
    virtual bool start(IOService *provider) override;
    virtual void free(void) override;

    virtual IOService *probe(IOService * provider, SInt32 *score) override;
    virtual bool configure(IOService * provider) override;

    bool allChildrenPoweredOn(void);
    IOPCIHostBridgeData *bridgeData;
    uint32_t _domainId;
protected:
    virtual IOReturn setLinkSpeed(tIOPCILinkSpeed, bool) override { return kIOReturnUnsupported; }
    virtual IOReturn getLinkSpeed(tIOPCILinkSpeed *) override { return kIOReturnUnsupported; }
private:
    bool childPublished(void *refcon, IOService *newService, IONotifier *notifier);
    IONotifier *_publishNotifier;
};

#endif

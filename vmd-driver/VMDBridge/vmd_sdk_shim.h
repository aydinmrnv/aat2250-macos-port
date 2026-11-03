/*
 * Compile shim for the MacOSX26.5 / 27.0 SDKs: IOKit/pwr_mgt/RootDomain.h and IOKit/pci/IOPCIBridge.h use
 * IOPMDriverAssertionID / Type / Level, but the SDK's IOPMAssertionTypes.h ships empty and no other SDK header
 * defines them (checked 2026-11-02). VMDBridge never calls IOPMrootDomain assertion APIs and never instantiates
 * IOPCI2PCIBridge (the only class with a member of these types), so the exact width is irrelevant to the code
 * that is generated. These typedefs are NOT taken from an Apple source and must not be used for anything else.
 */
#ifndef VMD_SDK_SHIM_H
#define VMD_SDK_SHIM_H
#include <stdint.h>
typedef uint64_t IOPMDriverAssertionID;
typedef uint64_t IOPMDriverAssertionType;
typedef uint32_t IOPMDriverAssertionLevel;
#endif

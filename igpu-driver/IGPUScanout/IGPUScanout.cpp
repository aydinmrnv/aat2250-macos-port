/*
 * IGPUScanout - phase G1 of the Intel iGPU (8086:7D67, display version 14) external-monitor work.
 *
 * The firmware leaves pipe A lit on DDI D (TC1, Thunderbolt DP tunnel through the WD19TB) at 3840x2160@60, scanning a
 * black 1920x1080 linear XRGB8888 plane at GGTT 0xFF800000 through pipe scaler 1 (IGPUProbe boot 1). This kext keeps
 * that pipe, transcoder, link and clocks exactly as the firmware left them and only points plane 1 at a buffer in
 * system memory that a user-space agent (lgmirror) fills with frames of a macOS virtual display.
 *
 * What it writes, and only after a client asks (nothing at boot):
 *  - GGTT PTEs for its own buffer, in a range it first checks is unused (all-zero or a uniform scratch fill), with the
 *    same PTE flag bits as the firmware's own framebuffer PTE. The previous PTEs are saved and put back on detach.
 *  - mode 0 ("fw"): PLANE_SURF only. Same size, stride and format as the firmware plane; the scaler stays on.
 *  - mode 1 ("4k"): additionally scaler 1 off, PIPESRC, PLANE_SIZE, PLANE_STRIDE for a native 3840x2160 plane.
 *    All original values are saved and restored on detach.
 * Detach happens on request, when the client closes, or when the agent dies. Pipe/transcoder/DDI/PLL are never touched.
 *
 * Opt-in: inert unless the boot-arg -igpuscanout is present; -igpuscanoutoff always disables it.
 * Register offsets follow Linux i915 as reference only; no code copied.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>

#define ISO_LOG(fmt, ...) IOLog("IGPUScanout: " fmt "\n", ##__VA_ARGS__)

namespace {
enum : uint32_t {
    kMapLen          = 0x1000000,      /* all of BAR0: registers + GGTT */
    kGGTTBase        = 0x800000,       /* GGTT PTEs live in the upper half of GTTMMADR */
    kFwSurf          = 0xFF800000,     /* firmware plane surface (boot 1) */
    kOurGGTT         = 0xF0000000,     /* our buffer's GGTT offset; checked free before use */
    kOurGGTTSpan     = 0x04000000,     /* 64 MB checked */

    rPIPESRC_A       = 0x6001C,
    rTRANSCONF_A     = 0x70008,
    rFRMCOUNT_A      = 0x70040,
    rPLANE_CTL_1A    = 0x70180,
    rPLANE_STRIDE_1A = 0x70188,
    rPLANE_POS_1A    = 0x7018C,
    rPLANE_SIZE_1A   = 0x70190,
    rPLANE_SURF_1A   = 0x7019C,
    rPLANE_OFFSET_1A = 0x701A4,
    rPLANE_WM0_1A    = 0x70240,
    rPLANE_WMTRANS_1A= 0x70268,
    rPLANE_BUFCFG_1A = 0x7027C,
    rPS1A_WIN_POS    = 0x68170,
    rPS1A_WIN_SZ     = 0x68174,
    rPS1A_CTRL       = 0x68180,
    rGFX_FLSH_CNTL   = 0x101008,
    rGMD_ID_DISPLAY  = 0x510A0,
};
constexpr uint64_t kPTEAddrMask = 0x00003FFFFFFFF000ULL;
}

class IGPUScanoutClient;

class IGPUScanout : public IOService {
    OSDeclareDefaultStructors(IGPUScanout)
    friend class IGPUScanoutClient;
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IOReturn getInfo(uint64_t *out, uint32_t count);
    IOReturn attach(IGPUScanoutClient *who, uint32_t mode, uint64_t *out, uint32_t count);
    IOReturn detach(IGPUScanoutClient *who);
    IOMemoryDescriptor *buffer() { return buf; }
    /* 0.0.3 development primitives (one owning client; everything is undone when it goes) */
    IOReturn regRead(uint32_t off, uint64_t *v);
    IOReturn regWrite(IGPUScanoutClient *who, uint32_t off, uint32_t v);
    IOReturn aux(uint32_t port, uint32_t sendLen, const uint64_t *din, uint64_t *o);
    IOReturn allocSlot(IGPUScanoutClient *who, uint32_t slot, uint32_t size, uint64_t *ggtt);
    IOMemoryDescriptor *slotBuffer(IGPUScanoutClient *who, uint32_t slot);
    void devCleanup(IGPUScanoutClient *who);

private:
    IOPCIDevice *pci = nullptr;
    IOMemoryMap *map = nullptr;
    volatile uint8_t *mmio = nullptr;
    IOLock *lock = nullptr;

    IGPUScanoutClient *owner = nullptr;
    IOBufferMemoryDescriptor *buf = nullptr;
    uint32_t mode = 0, width = 0, height = 0, strideBytes = 0, pages = 0;
    uint64_t *savedPTE = nullptr;
    uint32_t sv_surf = 0, sv_stride = 0, sv_size = 0, sv_pos = 0, sv_offset = 0, sv_pipesrc = 0;
    uint32_t sv_psctrl = 0, sv_pspos = 0, sv_pssz = 0;

    enum { kMaxUndo = 1024, kSlots = 4 };
    struct Undo { uint32_t off, val; };
    IGPUScanoutClient *devOwner = nullptr;
    Undo *undo = nullptr; uint32_t nUndo = 0;
    IOBufferMemoryDescriptor *xbuf[kSlots] = {};
    uint64_t *xsaved[kSlots] = {};
    uint32_t xpages[kSlots] = {};
    static uint32_t slotGGTT(uint32_t slot) { return kOurGGTT + slot * kOurGGTTSpan; }
    bool rangeFree(uint32_t base, uint32_t span) const;
    void devCleanupLocked();

    uint32_t rd(uint32_t off) const { return *(volatile uint32_t *)(mmio + off); }
    void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }
    uint64_t pte(uint32_t ggttOff) const { return *(volatile uint64_t *)(mmio + kGGTTBase + (ggttOff >> 12) * 8ULL); }
    void setPTE(uint32_t ggttOff, uint64_t v) { *(volatile uint64_t *)(mmio + kGGTTBase + (ggttOff >> 12) * 8ULL) = v; }
    void flushGGTT() { (void)pte(kOurGGTT); wr(rGFX_FLSH_CNTL, 1); (void)rd(rGFX_FLSH_CNTL); }
    bool waitFrames(unsigned n);
    bool pipeALit() const { return (rd(rTRANSCONF_A) & 0xC0000000u) == 0xC0000000u; }
    void releaseLocked();
};

class IGPUScanoutClient : public IOUserClient {
    OSDeclareDefaultStructors(IGPUScanoutClient)
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
private:
    IGPUScanout *svc = nullptr;
    static IOReturn sGetInfo(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sAttach(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sDetach(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sRegRead(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sRegWrite(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sAux(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sAlloc(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sCleanup(OSObject *t, void *, IOExternalMethodArguments *a);
    static const IOExternalMethodDispatch kMethods[8];
};

OSDefineMetaClassAndStructors(IGPUScanout, IOService)
OSDefineMetaClassAndStructors(IGPUScanoutClient, IOUserClient)

/* ---------------- service ---------------- */

IOService *IGPUScanout::probe(IOService *provider, SInt32 *score)
{
    int flag = 0;
    if (PE_parse_boot_argn("-igpuscanoutoff", &flag, sizeof flag)) return nullptr;
    if (!PE_parse_boot_argn("-igpuscanout", &flag, sizeof flag)) return nullptr;
    return IOService::probe(provider, score);
}

bool IGPUScanout::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;
    if (pci->configRead16(kIOPCIConfigVendorID) != 0x8086 || pci->configRead16(kIOPCIConfigDeviceID) != 0x7D67) {
        setProperty("ISOStatus", "unexpected device id");
        return false;
    }
    const uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
    if ((cmd & (kIOPCICommandMemorySpace | kIOPCICommandBusMaster)) != (kIOPCICommandMemorySpace | kIOPCICommandBusMaster)) {
        setProperty("ISOStatus", "memory decode or bus master off; this kext never turns them on");
        return false;
    }
    const uint32_t bar0 = pci->configRead32(kIOPCIConfigBaseAddress0), bar1 = pci->configRead32(kIOPCIConfigBaseAddress1);
    const uint64_t hw = ((bar0 & 0x6) == 0x4 ? ((uint64_t)bar1 << 32) : 0) | (bar0 & ~0xFull);
    IODeviceMemory *dm = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!dm || dm->getPhysicalAddress() != hw || dm->getLength() < kMapLen) {
        setProperty("ISOStatus", "BAR0 missing or OS/hardware mismatch");
        return false;
    }
    map = dm->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache);
    if (!map) { setProperty("ISOStatus", "could not map BAR0"); return false; }
    mmio = (volatile uint8_t *)map->getVirtualAddress();
    if (rd(rGMD_ID_DISPLAY) == 0xFFFFFFFFu) {
        setProperty("ISOStatus", "MMIO reads all-ones");
        map->release(); map = nullptr; mmio = nullptr;
        return false;
    }
    lock = IOLockAlloc();
    if (!lock) return false;
    undo = (Undo *)IOMalloc(sizeof(Undo) * kMaxUndo);
    if (!undo) return false;
    setProperty("IOUserClientClass", "IGPUScanoutClient");
    setProperty("ISOStatus", pipeALit() ? "ready (pipe A lit by firmware)" : "pipe A not lit; attach will refuse");
    setProperty("ISOFirmwareSurf", rd(rPLANE_SURF_1A), 32);
    registerService();
    ISO_LOG("ready; TRANSCONF_A %08x PLANE_SURF_1A %08x", rd(rTRANSCONF_A), rd(rPLANE_SURF_1A));
    return true;
}

void IGPUScanout::stop(IOService *provider)
{
    if (lock) {
        IOLockLock(lock);
        devCleanupLocked();
        releaseLocked();
        IOLockUnlock(lock);
    }
    if (map) { map->release(); map = nullptr; mmio = nullptr; }
    if (lock) { IOLockFree(lock); lock = nullptr; }
    if (undo) { IOFree(undo, sizeof(Undo) * kMaxUndo); undo = nullptr; }
    IOService::stop(provider);
}

bool IGPUScanout::waitFrames(unsigned n)
{
    const uint32_t start = rd(rFRMCOUNT_A);
    for (unsigned i = 0; i < 100 * n; i++) {          /* 60 Hz: one frame ~16.7 ms; give up after ~n*100 ms */
        if (rd(rFRMCOUNT_A) - start >= n) return true;
        IOSleep(1);
    }
    return false;
}

IOReturn IGPUScanout::getInfo(uint64_t *o, uint32_t count)
{
    if (count < 16) return kIOReturnBadArgument;
    const uint32_t fwIdxOff = rd(rPLANE_SURF_1A) & ~0xFFFu;
    o[0] = rd(rTRANSCONF_A);      o[1] = rd(rPIPESRC_A);        o[2] = rd(rPLANE_CTL_1A);
    o[3] = rd(rPLANE_STRIDE_1A);  o[4] = rd(rPLANE_SIZE_1A);    o[5] = rd(rPLANE_SURF_1A);
    o[6] = rd(rPS1A_CTRL);        o[7] = rd(rPLANE_BUFCFG_1A);  o[8] = rd(rPLANE_WM0_1A);
    o[9] = rd(rPLANE_WMTRANS_1A); o[10] = rd(rFRMCOUNT_A);      o[11] = pte(fwIdxOff);
    o[12] = pte(kOurGGTT);        o[13] = owner ? 1 : 0;        o[14] = mode;
    o[15] = rd(rGMD_ID_DISPLAY);
    return kIOReturnSuccess;
}

IOReturn IGPUScanout::attach(IGPUScanoutClient *who, uint32_t m, uint64_t *o, uint32_t count)
{
    if (count < 4 || m > 1) return kIOReturnBadArgument;
    IOLockLock(lock);
    IOReturn ret = kIOReturnSuccess;
    do {
        if (owner) { ret = kIOReturnExclusiveAccess; break; }
        if (!pipeALit()) { ret = kIOReturnNotReady; setProperty("ISOStatus", "attach refused: pipe A not lit"); break; }

        /* The firmware's own PTE tells us the GGTT layout and the flag bits that scan out on this part. */
        const uint32_t fwSurf = rd(rPLANE_SURF_1A) & ~0xFFFu;
        const uint64_t fwPTE = pte(fwSurf);
        if (!(fwPTE & 1) || !(fwPTE & kPTEAddrMask)) {
            ret = kIOReturnNoDevice; setProperty("ISOStatus", "attach refused: firmware PTE not valid (GGTT layout?)"); break;
        }
        /* 0.0.2: the firmware PTE (0x800003 on boot 1) carries bit 1, GEN12_GGTT_PTE_LM: its address is an offset into
         * stolen memory. Our pages are system RAM, so they get present (bit 0) and MTL PAT index 2 = uncached
         * (PAT1, bit 53), as i915 encodes a display buffer on Meteor Lake class parts. Never the LM bit. */
        const uint64_t flags = 1ULL | (1ULL << 53);
        setProperty("ISOFirmwarePTE", fwPTE, 64);

        /* Our range must be unused: all zero, or one uniform scratch page. */
        uint64_t firstValid = 0; bool used = false;
        for (uint32_t off = 0; off < kOurGGTTSpan; off += 0x1000) {
            const uint64_t e = pte(kOurGGTT + off);
            if (!(e & 1)) continue;
            if (!firstValid) firstValid = e;
            else if ((e & kPTEAddrMask) != (firstValid & kPTEAddrMask)) { used = true; break; }
        }
        if (used) { ret = kIOReturnBusy; setProperty("ISOStatus", "attach refused: GGTT range in use"); break; }

        if (m == 0) {   /* firmware plane geometry */
            width = (rd(rPLANE_SIZE_1A) & 0xFFFF) + 1;
            height = (rd(rPLANE_SIZE_1A) >> 16) + 1;
            strideBytes = (rd(rPLANE_STRIDE_1A) & 0x7FF) * 64;
        } else {        /* native 4K: the transcoder's active size */
            width = 3840; height = 2160; strideBytes = 3840 * 4;
        }
        if (width < 640 || height < 480 || width > 4096 || height > 4096 || strideBytes < width * 4) {
            ret = kIOReturnUnsupported; setProperty("ISOStatus", "attach refused: odd plane geometry"); break;
        }
        const uint32_t size = (strideBytes * height + 0xFFF) & ~0xFFFu;
        pages = size >> 12;
        if (size > kOurGGTTSpan) { ret = kIOReturnNoSpace; break; }

        buf = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
                kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache, size, 0x1000);
        if (!buf) { ret = kIOReturnNoMemory; break; }
        if (buf->prepare() != kIOReturnSuccess) { buf->release(); buf = nullptr; ret = kIOReturnNoMemory; break; }
        bzero(buf->getBytesNoCopy(), size);                  /* black */
        buf->performOperation(kIOMemoryIncoherentIOFlush, 0, size);

        savedPTE = (uint64_t *)IOMalloc(pages * sizeof(uint64_t));
        if (!savedPTE) { buf->complete(); buf->release(); buf = nullptr; ret = kIOReturnNoMemory; break; }
        bool ok = true;
        for (uint32_t i = 0; i < pages; i++) {
            savedPTE[i] = pte(kOurGGTT + i * 0x1000);
            IOByteCount segLen = 0;
            const addr64_t pa = buf->getPhysicalSegment((IOByteCount)i * 0x1000, &segLen, kIOMemoryMapperNone);
            if (!pa || (pa & 0xFFF) || (pa & ~kPTEAddrMask)) { ok = false; break; }
            setPTE(kOurGGTT + i * 0x1000, (pa & kPTEAddrMask) | flags);
        }
        if (!ok) {
            for (uint32_t i = 0; i < pages; i++) setPTE(kOurGGTT + i * 0x1000, savedPTE[i]);
            flushGGTT();
            IOFree(savedPTE, pages * sizeof(uint64_t)); savedPTE = nullptr;
            buf->complete(); buf->release(); buf = nullptr;
            ret = kIOReturnVMError; setProperty("ISOStatus", "attach refused: bad physical segment"); break;
        }
        flushGGTT();

        sv_surf = rd(rPLANE_SURF_1A); sv_stride = rd(rPLANE_STRIDE_1A); sv_size = rd(rPLANE_SIZE_1A);
        sv_pos = rd(rPLANE_POS_1A); sv_offset = rd(rPLANE_OFFSET_1A); sv_pipesrc = rd(rPIPESRC_A);
        sv_psctrl = rd(rPS1A_CTRL); sv_pspos = rd(rPS1A_WIN_POS); sv_pssz = rd(rPS1A_WIN_SZ);
        mode = m;
        waitFrames(1);                                       /* start right after a vblank */
        if (m == 1) {
            wr(rPS1A_CTRL, 0); wr(rPS1A_WIN_POS, 0); wr(rPS1A_WIN_SZ, 0);    /* WIN_SZ arms the scaler update */
            wr(rPIPESRC_A, ((width - 1) << 16) | (height - 1));
            wr(rPLANE_STRIDE_1A, strideBytes / 64);
            wr(rPLANE_SIZE_1A, ((height - 1) << 16) | (width - 1));
            wr(rPLANE_POS_1A, 0); wr(rPLANE_OFFSET_1A, 0);
        }
        wr(rPLANE_SURF_1A, kOurGGTT);                        /* arms the plane update at the next vblank */
        owner = who;
        const bool alive = waitFrames(2);
        setProperty("ISOPTEFlags", flags, 64); setProperty("ISOAttachPTE0", pte(kOurGGTT), 64);
        setProperty("ISOStatus", alive ? (m ? "attached (4k)" : "attached (fw geometry)") : "attached; frame counter stalled");
        ISO_LOG("attached mode %u %ux%u stride %u, %u pages at GGTT %08x, PTE flags %llx, pipe %s", m, width, height,
                strideBytes, pages, kOurGGTT, flags, alive ? "running" : "STALLED");
        o[0] = width; o[1] = height; o[2] = strideBytes; o[3] = kOurGGTT;
    } while (false);
    IOLockUnlock(lock);
    return ret;
}

void IGPUScanout::releaseLocked()
{
    if (!owner && !buf) return;
    if (mmio && owner) {
        waitFrames(1);
        if (mode == 1) {
            wr(rPS1A_CTRL, sv_psctrl); wr(rPS1A_WIN_POS, sv_pspos); wr(rPS1A_WIN_SZ, sv_pssz);
            wr(rPIPESRC_A, sv_pipesrc);
            wr(rPLANE_STRIDE_1A, sv_stride); wr(rPLANE_SIZE_1A, sv_size);
            wr(rPLANE_POS_1A, sv_pos); wr(rPLANE_OFFSET_1A, sv_offset);
        }
        wr(rPLANE_SURF_1A, sv_surf);
        if (!waitFrames(3)) IOSleep(100);                    /* the plane must be off our pages before they go */
        if (savedPTE) {
            for (uint32_t i = 0; i < pages; i++) setPTE(kOurGGTT + i * 0x1000, savedPTE[i]);
            flushGGTT();
        }
    }
    if (savedPTE) { IOFree(savedPTE, pages * sizeof(uint64_t)); savedPTE = nullptr; }
    if (buf) { buf->complete(); buf->release(); buf = nullptr; }
    owner = nullptr; pages = 0;
    setProperty("ISOStatus", "detached (firmware plane restored)");
    ISO_LOG("detached; firmware plane restored");
}

IOReturn IGPUScanout::detach(IGPUScanoutClient *who)
{
    IOLockLock(lock);
    IOReturn ret = kIOReturnSuccess;
    if (owner != who) ret = kIOReturnNotOpen;
    else releaseLocked();
    IOLockUnlock(lock);
    return ret;
}


/* ---------------- 0.0.3 development primitives ---------------- */

bool IGPUScanout::rangeFree(uint32_t base, uint32_t span) const
{
    uint64_t firstValid = 0;
    for (uint32_t off = 0; off < span; off += 0x1000) {
        const uint64_t e = pte(base + off);
        if (!(e & 1)) continue;
        if (!firstValid) firstValid = e;
        else if ((e & kPTEAddrMask) != (firstValid & kPTEAddrMask)) return false;
    }
    return true;
}

IOReturn IGPUScanout::regRead(uint32_t off, uint64_t *v)
{
    if ((off & 3) || off >= 0x200000) return kIOReturnBadArgument;
    *v = rd(off);
    return kIOReturnSuccess;
}

/* Display registers only: 0x40000-0x7FFFF (pipes, transcoders, planes, DDI, PLLs) and 0x160000-0x16FFFF (PICA, TC, AUX).
 * The first write to each offset records the value it had, and the owner's departure writes them back in reverse. */
IOReturn IGPUScanout::regWrite(IGPUScanoutClient *who, uint32_t off, uint32_t v)
{
    if ((off & 3) || !((off >= 0x40000 && off < 0x80000) || (off >= 0x160000 && off < 0x170000))) return kIOReturnNotPermitted;
    IOLockLock(lock);
    IOReturn ret = kIOReturnSuccess;
    if (devOwner && devOwner != who) ret = kIOReturnExclusiveAccess;
    else {
        devOwner = who;
        bool seen = false;
        for (uint32_t i = 0; i < nUndo; i++) if (undo[i].off == off) { seen = true; break; }
        if (!seen) {
            if (nUndo >= kMaxUndo) ret = kIOReturnNoSpace;
            else undo[nUndo++] = { off, rd(off) };
        }
        if (ret == kIOReturnSuccess) wr(off, v);
    }
    IOLockUnlock(lock);
    return ret;
}

/* One raw AUX transfer on a Type-C port's AUX channel (0 = USBC1 = TC1, the dock). Up to 20 bytes each way.
 * The caller builds the AUX header and interprets the reply; the static control bits (timeout, sync lengths,
 * power request, TBT IO) are taken from what the firmware left in the register. */
IOReturn IGPUScanout::aux(uint32_t port, uint32_t sendLen, const uint64_t *din, uint64_t *o)
{
    if (port > 3 || sendLen < 1 || sendLen > 20) return kIOReturnBadArgument;
    const uint32_t c = 0x16F210 + 0x200 * port;
    uint8_t b[20] = {0};
    for (uint32_t i = 0; i < sendLen; i++) b[i] = (uint8_t)(din[i / 8] >> ((i % 8) * 8));
    IOLockLock(lock);
    IOReturn ret = kIOReturnSuccess;
    uint32_t cur = rd(c);
    for (int i = 0; (cur & (1u << 31)) && i < 1000; i++) { IODelay(10); cur = rd(c); }
    if (cur & (1u << 31)) ret = kIOReturnBusy;
    else {
        for (uint32_t w = 0; w < 5; w++)
            wr(c + 4 + 4 * w, ((uint32_t)b[4 * w] << 24) | ((uint32_t)b[4 * w + 1] << 16) | ((uint32_t)b[4 * w + 2] << 8) | b[4 * w + 3]);
        const uint32_t ctl = (cur & (0x0C000000u | (1u << 19) | (1u << 11) | 0x3FFu)) |
                             (1u << 31) | (1u << 30) | (1u << 29) | (1u << 28) | (1u << 25) | (sendLen << 20);
        wr(c, ctl);
        uint32_t st = rd(c);
        for (int i = 0; (st & (1u << 31)) && i < 2000; i++) { IODelay(5); st = rd(c); }
        o[0] = st;
        const uint32_t rlen = (st >> 20) & 0x1F;
        o[1] = rlen;
        o[2] = o[3] = o[4] = 0;
        for (uint32_t w = 0; w < 5; w++) {
            const uint32_t word = rd(c + 4 + 4 * w);
            for (uint32_t k = 0; k < 4; k++) {
                const uint32_t i = 4 * w + k;
                o[2 + i / 8] |= (uint64_t)((word >> (24 - 8 * k)) & 0xFF) << ((i % 8) * 8);
            }
        }
        if (st & (1u << 31)) ret = kIOReturnTimeout;
    }
    IOLockUnlock(lock);
    return ret;
}

IOReturn IGPUScanout::allocSlot(IGPUScanoutClient *who, uint32_t slot, uint32_t size, uint64_t *ggtt)
{
    if (slot < 1 || slot >= kSlots || size == 0 || size > kOurGGTTSpan) return kIOReturnBadArgument;
    IOLockLock(lock);
    IOReturn ret = kIOReturnSuccess;
    do {
        if (devOwner && devOwner != who) { ret = kIOReturnExclusiveAccess; break; }
        if (xbuf[slot]) { ret = kIOReturnBusy; break; }
        const uint32_t base = slotGGTT(slot);
        if (!rangeFree(base, kOurGGTTSpan)) { ret = kIOReturnBusy; break; }
        size = (size + 0xFFF) & ~0xFFFu;
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
                kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache, size, 0x1000);
        if (!b) { ret = kIOReturnNoMemory; break; }
        if (b->prepare() != kIOReturnSuccess) { b->release(); ret = kIOReturnNoMemory; break; }
        bzero(b->getBytesNoCopy(), size);
        b->performOperation(kIOMemoryIncoherentIOFlush, 0, size);
        const uint32_t np = size >> 12;
        uint64_t *sv = (uint64_t *)IOMalloc(np * sizeof(uint64_t));
        if (!sv) { b->complete(); b->release(); ret = kIOReturnNoMemory; break; }
        bool ok = true;
        for (uint32_t i = 0; i < np; i++) {
            sv[i] = pte(base + i * 0x1000);
            IOByteCount segLen = 0;
            const addr64_t pa = b->getPhysicalSegment((IOByteCount)i * 0x1000, &segLen, kIOMemoryMapperNone);
            if (!pa || (pa & 0xFFF) || (pa & ~kPTEAddrMask)) { ok = false; break; }
            setPTE(base + i * 0x1000, (pa & kPTEAddrMask) | 1ULL | (1ULL << 53));
        }
        if (!ok) {
            for (uint32_t i = 0; i < np; i++) setPTE(base + i * 0x1000, sv[i]);
            flushGGTT(); IOFree(sv, np * sizeof(uint64_t)); b->complete(); b->release();
            ret = kIOReturnVMError; break;
        }
        flushGGTT();
        devOwner = who;
        xbuf[slot] = b; xsaved[slot] = sv; xpages[slot] = np;
        *ggtt = base;
    } while (false);
    IOLockUnlock(lock);
    return ret;
}

IOMemoryDescriptor *IGPUScanout::slotBuffer(IGPUScanoutClient *who, uint32_t slot)
{
    IOMemoryDescriptor *md = nullptr;
    IOLockLock(lock);
    if (slot == 0) md = (owner == who) ? buf : nullptr;
    else if (slot < kSlots && devOwner == who) md = xbuf[slot];
    if (md) md->retain();
    IOLockUnlock(lock);
    return md;
}

void IGPUScanout::devCleanupLocked()
{
    if (!devOwner && !nUndo) return;
    /* 1. No plane may keep scanning our extra slots once they are freed. */
    const uint32_t lo = slotGGTT(1), hi = slotGGTT(kSlots);
    for (uint32_t p = 0; p < 4; p++) {
        const uint32_t surf = rd(rPLANE_SURF_1A + 0x1000 * p) & ~0xFFFu;
        if (surf >= lo && surf < hi) { wr(rPLANE_CTL_1A + 0x1000 * p, 0); wr(rPLANE_SURF_1A + 0x1000 * p, 0); }
    }
    waitFrames(2);
    /* 2. Every logged register back to its first-seen value, newest first. */
    for (uint32_t i = nUndo; i-- > 0;) wr(undo[i].off, undo[i].val);
    nUndo = 0;
    waitFrames(3);
    /* 3. GGTT and memory. */
    for (uint32_t s = 1; s < kSlots; s++) {
        if (!xbuf[s]) continue;
        for (uint32_t i = 0; i < xpages[s]; i++) setPTE(slotGGTT(s) + i * 0x1000, xsaved[s][i]);
        flushGGTT();
        IOFree(xsaved[s], xpages[s] * sizeof(uint64_t)); xsaved[s] = nullptr;
        xbuf[s]->complete(); xbuf[s]->release(); xbuf[s] = nullptr; xpages[s] = 0;
    }
    devOwner = nullptr;
    ISO_LOG("development state undone");
}

void IGPUScanout::devCleanup(IGPUScanoutClient *who)
{
    IOLockLock(lock);
    if (devOwner == who) devCleanupLocked();
    IOLockUnlock(lock);
}

/* ---------------- user client ---------------- */

const IOExternalMethodDispatch IGPUScanoutClient::kMethods[8] = {
    { (IOExternalMethodAction)&IGPUScanoutClient::sGetInfo,  0, 0, 16, 0 },
    { (IOExternalMethodAction)&IGPUScanoutClient::sAttach,   1, 0, 4,  0 },
    { (IOExternalMethodAction)&IGPUScanoutClient::sDetach,   0, 0, 0,  0 },
    { (IOExternalMethodAction)&IGPUScanoutClient::sRegRead,  1, 0, 1,  0 },
    { (IOExternalMethodAction)&IGPUScanoutClient::sRegWrite, 2, 0, 0,  0 },
    { (IOExternalMethodAction)&IGPUScanoutClient::sAux,      5, 0, 5,  0 },
    { (IOExternalMethodAction)&IGPUScanoutClient::sAlloc,    2, 0, 1,  0 },
    { (IOExternalMethodAction)&IGPUScanoutClient::sCleanup,  0, 0, 0,  0 },
};

bool IGPUScanoutClient::initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties)
{
    /* Only the logged-in console user (or root). */
    if (clientHasPrivilege(securityID, kIOClientPrivilegeLocalUser) != kIOReturnSuccess &&
        clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return false;
    return IOUserClient::initWithTask(owningTask, securityID, type, properties);
}

bool IGPUScanoutClient::start(IOService *provider)
{
    svc = OSDynamicCast(IGPUScanout, provider);
    return svc && IOUserClient::start(provider);
}

IOReturn IGPUScanoutClient::clientClose()
{
    if (svc) { svc->devCleanup(this); svc->detach(this); }
    terminate();
    return kIOReturnSuccess;
}

IOReturn IGPUScanoutClient::clientDied()
{
    if (svc) { svc->devCleanup(this); svc->detach(this); }
    return IOUserClient::clientDied();
}

IOReturn IGPUScanoutClient::sGetInfo(OSObject *t, void *, IOExternalMethodArguments *a)
{
    return ((IGPUScanoutClient *)t)->svc->getInfo(a->scalarOutput, a->scalarOutputCount);
}

IOReturn IGPUScanoutClient::sAttach(OSObject *t, void *, IOExternalMethodArguments *a)
{
    IGPUScanoutClient *c = (IGPUScanoutClient *)t;
    return c->svc->attach(c, (uint32_t)a->scalarInput[0], a->scalarOutput, a->scalarOutputCount);
}

IOReturn IGPUScanoutClient::sDetach(OSObject *t, void *, IOExternalMethodArguments *)
{
    IGPUScanoutClient *c = (IGPUScanoutClient *)t;
    return c->svc->detach(c);
}

IOReturn IGPUScanoutClient::sRegRead(OSObject *t, void *, IOExternalMethodArguments *a)
{
    return ((IGPUScanoutClient *)t)->svc->regRead((uint32_t)a->scalarInput[0], &a->scalarOutput[0]);
}

IOReturn IGPUScanoutClient::sRegWrite(OSObject *t, void *, IOExternalMethodArguments *a)
{
    IGPUScanoutClient *c = (IGPUScanoutClient *)t;
    return c->svc->regWrite(c, (uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1]);
}

IOReturn IGPUScanoutClient::sAux(OSObject *t, void *, IOExternalMethodArguments *a)
{
    return ((IGPUScanoutClient *)t)->svc->aux((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1],
                                              &a->scalarInput[2], a->scalarOutput);
}

IOReturn IGPUScanoutClient::sAlloc(OSObject *t, void *, IOExternalMethodArguments *a)
{
    IGPUScanoutClient *c = (IGPUScanoutClient *)t;
    return c->svc->allocSlot(c, (uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1], &a->scalarOutput[0]);
}

IOReturn IGPUScanoutClient::sCleanup(OSObject *t, void *, IOExternalMethodArguments *)
{
    IGPUScanoutClient *c = (IGPUScanoutClient *)t;
    c->svc->devCleanup(c);
    return kIOReturnSuccess;
}

IOReturn IGPUScanoutClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                           IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    if (selector >= 8) return kIOReturnBadArgument;
    dispatch = (IOExternalMethodDispatch *)&kMethods[selector];
    return IOUserClient::externalMethod(selector, args, dispatch, this, reference);
}

IOReturn IGPUScanoutClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
    if (!svc) return kIOReturnBadArgument;
    IOMemoryDescriptor *md = svc->slotBuffer(this, type);
    if (!md) return kIOReturnNotReady;
    *options = kIOMapWriteCombineCache;   /* the display engine does not snoop the CPU cache */
    *memory = md;
    return kIOReturnSuccess;
}

/*
 * IGPUProbe - phase G0 of the Intel iGPU (8086:7D67, Arrow Lake-S, display version 14) external-monitor work.
 *
 * READ-ONLY. It never writes PCI config space or MMIO, never touches BAR2 (stolen-memory/aperture access through a
 * BAR can hang Meteor Lake class parts) and never takes forcewake. It maps the first 2 MB of BAR0 (GTTMMADR,
 * display registers live below 0x170000) uncached and read-only, reads the registers that say what the firmware
 * left lit on each pipe, transcoder and DDI port, publishes them on its own IORegistry node and unmaps again.
 *
 * Question it answers: when OpenCore hands over, are the two LG monitors on the Type-C/Thunderbolt DDIs already
 * driven (transcoder enabled, link trained, timing set), and does any plane scan out? If yes, a framebuffer that
 * adopts the firmware's mode (the NMIntelFB / XeGfxIntelFB approach on laptop panels) may work here. If no, the
 * full modeset path (Type-C PHY, PLL, link training) is needed.
 *
 * Opt-in: does nothing unless the boot-arg -igpuprobe is present; -igpuoff always disables it.
 * Register offsets follow Linux i915 (display/intel_display_regs.h, intel_tc.c, MTL/XE_LPD+ paths) as reference only;
 * no code copied. Everything is also published raw (offset -> value) so the decode can be redone later.
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

#define IGP_LOG(fmt, ...) IOLog("IGPUProbe: " fmt "\n", ##__VA_ARGS__)

enum : uint32_t {
    kIGPMapLen = 0x200000,          /* 2 MB of BAR0: every display register this probe reads */
    kIGPPipes  = 4,                 /* A-D */
    kIGPPorts  = 9,                 /* DDI A..I (TC1..TC4 are D..G) */
};

/* Single registers (offset, name). */
struct IGPReg { uint32_t off; const char *name; };
static const IGPReg kGlobalRegs[] = {
    { 0x510A0, "GMD_ID_DISPLAY" },
    { 0x45400, "PWR_WELL_CTL_BIOS" },  { 0x45404, "PWR_WELL_CTL_DRIVER" },
    { 0x45408, "PWR_WELL_CTL_KVMR" },  { 0x4540C, "PWR_WELL_CTL_DEBUG" },
    { 0x45500, "PWR_WELL_CTL_DDI_BIOS" }, { 0x45504, "PWR_WELL_CTL_DDI_DRIVER" },
    { 0x45480, "PWR_WELL_CTL_AUX_BIOS" }, { 0x45484, "PWR_WELL_CTL_AUX_DRIVER" },
    { 0x45008, "DBUF_CTL_S0" },        { 0x46000, "CDCLK_CTL" },
    { 0x46010, "DPLL0_ENABLE" },       { 0x46014, "DPLL1_ENABLE" },
    { 0x46018, "DPLL2_ENABLE" },       { 0x4601C, "DPLL3_ENABLE" },
    { 0x46020, "TBT_PLL_ENABLE" },
    { 0x44470, "DE_HPD_ISR" },         { 0x44474, "DE_HPD_IMR" },
    { 0x44478, "DE_HPD_IIR" },         { 0x4447C, "DE_HPD_IER" },
    { 0x44440, "DE_PORT_ISR" },
    { 0x16FE00, "PICAINTERRUPT_ISR" }, { 0x16FE04, "PICAINTERRUPT_IMR" },
    { 0x161500, "TCSS_DDI_STATUS_1" }, { 0x161504, "TCSS_DDI_STATUS_2" },
    { 0x161508, "TCSS_DDI_STATUS_3" }, { 0x16150C, "TCSS_DDI_STATUS_4" },
    { 0x163000, "HOTPLUG_CTL_TC" },    { 0x163004, "HOTPLUG_CTL_TBT" },
};

/* Raw windows dumped dword by dword (start, end exclusive, name). */
struct IGPRange { uint32_t start, end; const char *name; };
static const IGPRange kRanges[] = {
    { 0x16F200, 0x16F300, "XELPDP_PORT_USBC" },   /* PORT_BUF_CTL1/2/3, CLOCK_CTL per Type-C port */
    { 0x64000,  0x64900,  "DDI_PORTS" },          /* DDI_BUF_CTL + per-port block for ports A..I */
};

static const char *const kPipeNames[] = { "A", "B", "C", "D" };
static const char *const kModeNames[] = { "HDMI", "DVI", "DP-SST", "DP-MST", "?", "?", "?", "?" };

class IGPUProbe : public IOService {
    OSDeclareDefaultStructors(IGPUProbe)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
private:
    volatile uint8_t *base = nullptr;
    uint32_t rd(uint32_t off) const { return *(volatile uint32_t *)(base + off); }
    static void putNum(OSDictionary *d, const char *key, uint64_t v, unsigned bits) {
        OSNumber *n = OSNumber::withNumber(v, bits);
        if (n) { d->setObject(key, n); n->release(); }
    }
    static void putStr(OSDictionary *d, const char *key, const char *v) {
        OSString *s = OSString::withCString(v);
        if (s) { d->setObject(key, s); s->release(); }
    }
    static void hexKey(char *buf, uint32_t off) { snprintf(buf, 12, "0x%06x", off); }
    OSDictionary *describePipe(unsigned p);
    OSDictionary *describePort(unsigned port);
};

OSDefineMetaClassAndStructors(IGPUProbe, IOService)

IOService *IGPUProbe::probe(IOService *provider, SInt32 *score)
{
    int flag = 0;
    if (PE_parse_boot_argn("-igpuoff", &flag, sizeof flag)) return nullptr;
    if (!PE_parse_boot_argn("-igpuprobe", &flag, sizeof flag)) return nullptr;
    return IOService::probe(provider, score);
}

/* One pipe + its transcoder (pipe n uses transcoder n on this display version). */
OSDictionary *IGPUProbe::describePipe(unsigned p)
{
    OSDictionary *d = OSDictionary::withCapacity(24);
    if (!d) return nullptr;
    const uint32_t t = 0x1000 * p;
    const uint32_t transconf = rd(0x70008 + t), ddiFunc = rd(0x60400 + t);
    const uint32_t htotal = rd(0x60000 + t), vtotal = rd(0x6000C + t), pipesrc = rd(0x6001C + t);
    const uint32_t planeCtl = rd(0x70180 + t), planeSurf = rd(0x7019C + t);
    const uint32_t planeStride = rd(0x70188 + t), planeSize = rd(0x70190 + t);

    putStr(d, "pipe", kPipeNames[p]);
    putNum(d, "TRANSCONF", transconf, 32);
    putNum(d, "TRANS_DDI_FUNC_CTL", ddiFunc, 32);
    putNum(d, "TRANS_DDI_FUNC_CTL2", rd(0x60404 + t), 32);
    putNum(d, "TRANS_CLK_SEL", rd(0x46140 + 4 * p), 32);
    putNum(d, "TRANS_HTOTAL", htotal, 32);   putNum(d, "TRANS_HBLANK", rd(0x60004 + t), 32);
    putNum(d, "TRANS_HSYNC", rd(0x60008 + t), 32);
    putNum(d, "TRANS_VTOTAL", vtotal, 32);   putNum(d, "TRANS_VBLANK", rd(0x60010 + t), 32);
    putNum(d, "TRANS_VSYNC", rd(0x60014 + t), 32);
    putNum(d, "PIPESRC", pipesrc, 32);
    putNum(d, "TRANS_DATA_M1", rd(0x60030 + t), 32); putNum(d, "TRANS_DATA_N1", rd(0x60034 + t), 32);
    putNum(d, "TRANS_LINK_M1", rd(0x60040 + t), 32); putNum(d, "TRANS_LINK_N1", rd(0x60044 + t), 32);
    putNum(d, "TRANS_MSA_MISC", rd(0x60410 + t), 32);
    putNum(d, "DP_TP_CTL", rd(0x60540 + t), 32);    putNum(d, "DP_TP_STATUS", rd(0x60544 + t), 32);
    putNum(d, "PIPEMISC", rd(0x70030 + t), 32);
    putNum(d, "PLANE1_CTL", planeCtl, 32);          putNum(d, "PLANE1_STRIDE", planeStride, 32);
    putNum(d, "PLANE1_POS", rd(0x7018C + t), 32);   putNum(d, "PLANE1_SIZE", planeSize, 32);
    putNum(d, "PLANE1_SURF", planeSurf, 32);        putNum(d, "PLANE1_OFFSET", rd(0x701A4 + t), 32);
    putNum(d, "PS1_CTRL", rd(0x68180 + 0x800 * p), 32);

    /* Decoded summary (display version 12+ field layout). */
    const bool transOn = transconf & (1u << 31), ddiOn = ddiFunc & (1u << 31), planeOn = planeCtl & (1u << 31);
    const unsigned ddiSel = (ddiFunc >> 27) & 0xF;                  /* 0 = none, else port index + 1 */
    const unsigned mode = (ddiFunc >> 24) & 0x7;                    /* 0 HDMI, 1 DVI, 2 DP SST, 3 DP MST */
    putNum(d, "decoded-transcoder-enabled", transOn, 8);
    putNum(d, "decoded-transcoder-active", (transconf >> 30) & 1, 8);
    putNum(d, "decoded-ddi-func-enabled", ddiOn, 8);
    putNum(d, "decoded-ddi-port", ddiSel ? ddiSel - 1 : 0xFF, 8);
    putStr(d, "decoded-mode", kModeNames[mode]);
    putNum(d, "decoded-h-active", (htotal & 0xFFFF) + 1, 32); putNum(d, "decoded-h-total", (htotal >> 16) + 1, 32);
    putNum(d, "decoded-v-active", (vtotal & 0xFFFF) + 1, 32); putNum(d, "decoded-v-total", (vtotal >> 16) + 1, 32);
    putNum(d, "decoded-src-w", (pipesrc >> 16) + 1, 32);      putNum(d, "decoded-src-h", (pipesrc & 0xFFFF) + 1, 32);
    putNum(d, "decoded-plane-enabled", planeOn, 8);
    putNum(d, "decoded-plane-format", (planeCtl >> 23) & 0x1F, 8);
    putNum(d, "decoded-plane-ggtt-offset", planeSurf & ~0xFFFu, 32);

    IGP_LOG("pipe %c: trans %s ddi %s port %d %s %ux%u (src %ux%u) plane %s surf %08x", 'A' + p,
            transOn ? "on" : "off", ddiOn ? "on" : "off", ddiSel ? (int)ddiSel - 1 : -1,
            kModeNames[mode],
            (htotal & 0xFFFF) + 1, (vtotal & 0xFFFF) + 1, (pipesrc >> 16) + 1, (pipesrc & 0xFFFF) + 1,
            planeOn ? "on" : "off", planeSurf);
    return d;
}

OSDictionary *IGPUProbe::describePort(unsigned port)
{
    OSDictionary *d = OSDictionary::withCapacity(4);
    if (!d) return nullptr;
    const uint32_t buf = rd(0x64000 + 0x100 * port);
    putNum(d, "port", port, 8);
    putNum(d, "DDI_BUF_CTL", buf, 32);
    putNum(d, "decoded-buf-enabled", buf >> 31, 8);
    putNum(d, "decoded-buf-idle", (buf >> 7) & 1, 8);
    return d;
}

bool IGPUProbe::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;

    const uint16_t ven = pci->configRead16(kIOPCIConfigVendorID), dev = pci->configRead16(kIOPCIConfigDeviceID);
    const uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
    setProperty("IGPCommand", cmd, 16);
    setProperty("IGPDeviceID", dev, 16);
    if (ven != 0x8086 || dev != 0x7D67) { setProperty("IGPStatus", "unexpected device id; not reading"); return true; }
    if (!(cmd & kIOPCICommandMemorySpace)) {
        setProperty("IGPStatus", "memory decode off; not reading (this probe never turns it on)");
        IGP_LOG("memory decode off (cmd %04x)", cmd);
        return true;
    }

    const uint32_t bar0 = pci->configRead32(kIOPCIConfigBaseAddress0), bar1 = pci->configRead32(kIOPCIConfigBaseAddress1);
    const uint64_t hwPhys = ((bar0 & 0x6) == 0x4 ? ((uint64_t)bar1 << 32) : 0) | (bar0 & ~0xFull);
    setProperty("IGPBAR0HWPhys", hwPhys, 64);
    IODeviceMemory *mmio = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (mmio) {
        setProperty("IGPBAR0OSPhys", mmio->getPhysicalAddress(), 64);
        setProperty("IGPBAR0Len", mmio->getLength(), 64);
    }
    if (!hwPhys || (mmio && (mmio->getPhysicalAddress() != hwPhys || mmio->getLength() < kIGPMapLen))) {
        setProperty("IGPStatus", "BAR0 missing or OS/hardware address mismatch; not reading");
        return true;
    }
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)hwPhys, kIGPMapLen, kIODirectionIn);
    IOMemoryMap *map = md ? md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly)
                          : nullptr;
    OSSafeReleaseNULL(md);
    if (!map) { setProperty("IGPStatus", "could not map BAR0"); return true; }
    base = (volatile uint8_t *)map->getVirtualAddress();

    /* A register that reads all-ones means the device is not answering (powered down or decode broken). */
    const uint32_t gmd = rd(0x510A0);
    if (gmd == 0xFFFFFFFFu) {
        setProperty("IGPStatus", "MMIO reads all-ones; not reading further");
        map->release(); base = nullptr; return true;
    }

    OSDictionary *globals = OSDictionary::withCapacity(sizeof kGlobalRegs / sizeof kGlobalRegs[0]);
    for (const IGPReg &r : kGlobalRegs) if (globals) putNum(globals, r.name, rd(r.off), 32);
    if (globals) { setProperty("IGPGlobals", globals); globals->release(); }

    OSArray *pipes = OSArray::withCapacity(kIGPPipes);
    for (unsigned p = 0; pipes && p < kIGPPipes; p++) {
        OSDictionary *e = describePipe(p);
        if (e) { pipes->setObject(e); e->release(); }
    }
    if (pipes) { setProperty("IGPPipes", pipes); pipes->release(); }

    OSArray *ports = OSArray::withCapacity(kIGPPorts);
    for (unsigned port = 0; ports && port < kIGPPorts; port++) {
        OSDictionary *e = describePort(port);
        if (e) { ports->setObject(e); e->release(); }
    }
    if (ports) { setProperty("IGPPorts", ports); ports->release(); }

    for (const IGPRange &r : kRanges) {
        OSDictionary *raw = OSDictionary::withCapacity((r.end - r.start) / 4);
        char key[12];
        for (uint32_t off = r.start; raw && off < r.end; off += 4) {
            const uint32_t v = rd(off);
            if (!v) continue;                                   /* keep the property small: zeros are implied */
            hexKey(key, off);
            putNum(raw, key, v, 32);
        }
        if (raw) { setProperty(r.name, raw); raw->release(); }
    }

    setProperty("IGPStatus", "read (raw values; decode per display version 14)");
    IGP_LOG("GMD_ID_DISPLAY %08x, PWR_WELL_CTL_BIOS %08x, DDI_BUF_CTL TC1..4 %08x %08x %08x %08x", gmd, rd(0x45400),
            rd(0x64300), rd(0x64400), rd(0x64500), rd(0x64600));
    map->release();   /* nothing stays mapped */
    base = nullptr;
    return true;
}

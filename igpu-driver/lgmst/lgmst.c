/*
 * lgmst - DisplayPort MST bring-up on the iGPU's TC1 Thunderbolt DP tunnel, for the two LG monitors behind the
 * WD19TB's Synaptics MST hub (DPCD 1.4, HBR3 x4, MSTM_CAP 1, SINK_COUNT 2; read 2026-11-04).
 *
 * The firmware leaves one SST stream (transcoder A -> DDI D) that the hub clones to both outputs. This tool:
 *   1. stops transcoder A and retrains the link in MST mode (TBT mode: the PHY belongs to the TBT controller,
 *      so training is only the DPCD/pattern handshake),
 *   2. discovers the hub's output ports with LINK_ADDRESS,
 *   3. allocates one VC payload per monitor (transcoder A -> pipe A, transcoder C -> pipe C, which owns DBUF S3/S4),
 *   4. puts a test pattern (or lgmirror's frames, later) on each.
 * Everything register-side goes through IGPUScanout's logged writes; on exit it tears MST down, retrains SST and lets
 * the kext undo the rest, which gives the firmware's cloned SST picture back.
 *
 * Usage: lgmst [--bpp-a 24|18] [--bpp-c 24|18] [--discover]
 * Register meanings follow Linux i915 / DRM MST helpers (read as reference, no code copied).
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static io_connect_t C;
static volatile sig_atomic_t gStop;
static void onSig(int s) { (void)s; gStop = 1; }

/* ---------- kext primitives ---------- */
static uint32_t R(uint32_t off) { uint64_t in = off, out = 0; uint32_t n = 1; IOConnectCallScalarMethod(C, 3, &in, 1, &out, &n); return (uint32_t)out; }
static int W(uint32_t off, uint32_t v) {
    uint64_t in[2] = { off, v };
    int kr = IOConnectCallScalarMethod(C, 4, in, 2, NULL, NULL);
    if (kr) fprintf(stderr, "  write %06x refused (0x%x)\n", off, kr);
    return kr;
}
static void RMW(uint32_t off, uint32_t clr, uint32_t set) { W(off, (R(off) & ~clr) | set); }

static int auxRaw(const uint8_t *tx, int txLen, uint8_t *rx) {
    uint64_t in[5] = { 0, (uint64_t)txLen, 0, 0, 0 }, out[5] = {0}; uint32_t n = 5;
    for (int i = 0; i < txLen; i++) in[2 + i / 8] |= (uint64_t)tx[i] << ((i % 8) * 8);
    if (IOConnectCallScalarMethod(C, 5, in, 5, out, &n)) return -1;
    int len = (int)out[1];
    for (int i = 0; i < len && i < 20; i++) rx[i] = (uint8_t)(out[2 + i / 8] >> ((i % 8) * 8));
    if (out[0] & ((1u << 28) | (1u << 25))) return -1;
    return len;
}
static int dpcdRead(uint32_t a, uint8_t *b, int len) {
    uint8_t tx[4] = { (uint8_t)(0x90 | ((a >> 16) & 0xF)), (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(len - 1) };
    for (int t = 0; t < 32; t++) {
        uint8_t rx[20]; int n = auxRaw(tx, 4, rx);
        if (n < 1) { usleep(1000); continue; }
        if ((rx[0] >> 4) == 2) { usleep(500); continue; }
        if ((rx[0] >> 4) != 0) return -2;
        memcpy(b, rx + 1, n - 1); return n - 1;
    }
    return -1;
}
static int dpcdWrite(uint32_t a, const uint8_t *b, int len) {
    uint8_t tx[20] = { (uint8_t)(0x80 | ((a >> 16) & 0xF)), (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(len - 1) };
    memcpy(tx + 4, b, len);
    for (int t = 0; t < 32; t++) {
        uint8_t rx[20]; int n = auxRaw(tx, 4 + len, rx);
        if (n < 1) { usleep(1000); continue; }
        if ((rx[0] >> 4) == 2) { usleep(500); continue; }
        if ((rx[0] >> 4) != 0) return -2;
        return len;
    }
    return -1;
}
static int dpcdW1(uint32_t a, uint8_t v) { return dpcdWrite(a, &v, 1); }
static int dpcdR1(uint32_t a) { uint8_t v; return dpcdRead(a, &v, 1) == 1 ? v : -1; }
static int dpcdWriteLong(uint32_t a, const uint8_t *b, int len) {
    for (int off = 0; off < len; off += 16) if (dpcdWrite(a + off, b + off, len - off < 16 ? len - off : 16) < 0) return -1;
    return len;
}
static int dpcdReadLong(uint32_t a, uint8_t *b, int len) {
    for (int off = 0; off < len; off += 16) if (dpcdRead(a + off, b + off, len - off < 16 ? len - off : 16) < 0) return -1;
    return len;
}

/* ---------- registers (display version 14) ---------- */
enum {
    TRANS_A = 0x0000, TRANS_C = 0x2000,                     /* transcoder block offsets */
    rHTOTAL = 0x60000, rVSYNCSHIFT = 0x60028, rDATA_M1 = 0x60030, rDATA_N1 = 0x60034, rLINK_M1 = 0x60040, rLINK_N1 = 0x60044,
    rDDI_FUNC = 0x60400, rMSA_MISC = 0x60410, rDP_TP_CTL = 0x60540, rDP_TP_STATUS = 0x60544,
    rPIPESRC = 0x6001C, rTRANSCONF = 0x70008, rPIPEMISC = 0x70030, rFRMCOUNT = 0x70040,
    rPLANE_CTL = 0x70180, rPLANE_STRIDE = 0x70188, rPLANE_POS = 0x7018C, rPLANE_SIZE = 0x70190, rPLANE_SURF = 0x7019C,
    rPLANE_OFFSET = 0x701A4, rPLANE_WM = 0x70240, rPLANE_WMTRANS = 0x70268, rPLANE_BUFCFG = 0x7027C,
    rPS1_WIN_POS = 0x68170, rPS1_WIN_SZ = 0x68174, rPS1_CTRL = 0x68180,
    rDDI_BUF_D = 0x64300, rCLK_SEL_A = 0x46140, rCLK_SEL_C = 0x46148,
};
#define TP_ENABLE   (1u << 31)
#define TP_MST      (1u << 27)
#define TP_ENH      (1u << 18)
#define TP_PAT(p)   ((uint32_t)(p) << 8)
#define TP_PAT_MASK (7u << 8)
enum { PAT1 = 0, PAT2 = 1, PAT_IDLE = 2, PAT_NORMAL = 3, PAT3 = 4 };

static void waitFrames(uint32_t pipeOff, int n) {
    uint32_t s = R(rFRMCOUNT + pipeOff);
    for (int i = 0; i < 100 * n && R(rFRMCOUNT + pipeOff) - s < (uint32_t)n; i++) usleep(1000);
}

/* ---------- link training over the TBT tunnel ---------- */
static int linkStatusOK(int needEq) {
    uint8_t s[3];
    if (dpcdRead(0x202, s, 3) != 3) return 0;
    const uint8_t want = needEq ? 0x7 : 0x1;
    for (int l = 0; l < 4; l++) if (((s[l / 2] >> (4 * (l & 1))) & want) != want) return 0;
    return needEq ? (s[2] & 1) : 1;
}

static int train(int mst) {
    const uint32_t base = R(rDP_TP_CTL) & ~(TP_ENABLE | TP_MST | TP_PAT_MASK);
    /* source: training pattern 1, then the buffer */
    W(rDP_TP_CTL, base | TP_ENABLE | TP_ENH | (mst ? TP_MST : 0) | TP_PAT(PAT1));
    W(rDDI_BUF_D, R(rDDI_BUF_D) | (1u << 31));
    for (int i = 0; i < 20 && (R(rDDI_BUF_D) & (1u << 7)); i++) usleep(500);
    /* sink: HBR3 x4, enhanced framing, TPS1 with scrambling off */
    uint8_t cfg[2] = { 0x1E, 0x84 }; dpcdWriteLong(0x100, cfg, 2);
    uint8_t tp[5] = { 0x21, 0, 0, 0, 0 }; dpcdWriteLong(0x102, tp, 5);
    int ok = 0;
    for (int i = 0; i < 10 && !ok; i++) { usleep(200); ok = linkStatusOK(0); }
    /* Through the TBT DP tunnel the lanes report nothing during TPS1 (seen 2026-11-04) and lock in TPS3, so CR is advisory. */
    printf("  clock recovery: %s\n", ok ? "ok" : "not reported (TBT tunnel), continuing");
    /* channel EQ with TPS3 (sink 0x002 bit 6) */
    RMW(rDP_TP_CTL, TP_PAT_MASK, TP_PAT(PAT3));
    dpcdW1(0x102, 0x23);
    ok = 0;
    for (int i = 0; i < 10 && !ok; i++) { usleep(16000); ok = linkStatusOK(1); }
    printf("  channel EQ: %s\n", ok ? "ok" : "FAILED");
    if (!ok) return -1;
    RMW(rDP_TP_CTL, TP_PAT_MASK, TP_PAT(PAT_IDLE));
    for (int i = 0; i < 20 && !(R(rDP_TP_STATUS) & (1u << 25)); i++) usleep(500);
    dpcdW1(0x102, 0x00);
    RMW(rDP_TP_CTL, TP_PAT_MASK, TP_PAT(PAT_NORMAL));
    usleep(1000);
    ok = linkStatusOK(1);
    printf("  link %s in %s mode, DP_TP_CTL %08x\n", ok ? "trained" : "LOST", mst ? "MST" : "SST", R(rDP_TP_CTL));
    return ok ? 0 : -1;
}

static void stopLink(void) {
    W(rTRANSCONF + TRANS_C, 0);
    RMW(rDDI_FUNC + TRANS_C, (1u << 31) | (1u << 8), 0);
    W(rTRANSCONF + TRANS_A, 0);
    for (int i = 0; i < 50 && (R(rTRANSCONF + TRANS_A) & (1u << 30)); i++) usleep(1000);
    RMW(rDDI_FUNC + TRANS_A, (1u << 31) | (1u << 8), 0);
    RMW(rDP_TP_CTL, TP_ENABLE, 0);
    RMW(rDDI_BUF_D, 1u << 31, 0);
    for (int i = 0; i < 20 && !(R(rDDI_BUF_D) & (1u << 7)); i++) usleep(500);
}

/* ---------- MST sideband messages (DP 1.2 section 2.11) ---------- */
static uint8_t crc4(const uint8_t *d, int nibbles) {
    uint8_t bm = 0x80, bs = 7, idx = 0, r = 0; int bits = nibbles * 4;
    while (bits--) { r <<= 1; r |= (d[idx] & bm) >> bs; bm >>= 1; bs--; if (!bm) { bm = 0x80; bs = 7; idx++; } if (r & 0x10) r ^= 0x13; }
    for (int i = 0; i < 4; i++) { r <<= 1; if (r & 0x10) r ^= 0x13; }
    return r & 0xF;
}
static uint8_t crc8(const uint8_t *d, int n) {
    uint8_t bm = 0x80, bs = 7; int idx = 0, bits = n * 8; uint16_t r = 0;
    while (bits--) { r <<= 1; r |= (d[idx] & bm) >> bs; bm >>= 1; bs--; if (!bm) { bm = 0x80; bs = 7; idx++; } if (r & 0x100) r ^= 0xD5; }
    for (int i = 0; i < 8; i++) { r <<= 1; if (r & 0x100) r ^= 0xD5; }
    return r & 0xFF;
}
static int seqno;

/* The hub sends CONNECTION_STATUS_NOTIFY etc. as up requests when UP_REQ_EN is set; read them and ACK. */
static void drainUpReq(void) {
    int v = dpcdR1(0x2003);
    if (v < 0 || !(v & 0x20)) return;
    uint8_t h[48];
    if (dpcdReadLong(0x1600, h, 16) < 0) return;
    const int lct = h[0] >> 4, hl = 1 + lct / 2 + 2, bl = h[1 + lct / 2] & 0x3F;
    if (hl + bl > 16 && hl + bl <= 48) dpcdReadLong(0x1600 + 16, h + 16, hl + bl - 16);
    const uint8_t req = h[hl] & 0x7F, sq = (h[hl - 1] >> 4) & 1;
    printf("  [hub up-request %02x", req);
    if (req == 0x02 && bl >= 20) printf(" CSN port %d ddps %d", h[hl + 1] >> 4, (h[hl + 18] >> 3) & 1);
    printf("]\n");
    dpcdW1(0x2003, 0x20);
    uint8_t m[8]; int i = 0;
    m[i++] = (1 << 4); m[i++] = 2; m[i++] = (uint8_t)((1 << 7) | (1 << 6) | (sq << 4));
    m[i - 1] |= crc4(m, i * 2 - 1);
    m[i++] = req; m[i] = crc8(&m[i - 1], 1); i++;
    dpcdWriteLong(0x1200, m, i);
}

/* Send one request to the hub itself (LCT 1) and collect the reply body (without CRCs). */
static int sideband(const uint8_t *body, int blen, int pathMsg, int broadcast, uint8_t *rep, int repMax) {
    uint8_t m[48]; int i = 0;
    m[i++] = (1 << 4) | (broadcast ? 6 : 0);                 /* LCT 1, LCR 0 (6 for broadcast) */
    m[i++] = (uint8_t)((broadcast << 7) | (pathMsg << 6) | ((blen + 1) & 0x3F));
    m[i++] = (uint8_t)((1 << 7) | (1 << 6) | (seqno << 4));  /* SOMT, EOMT */
    m[i - 1] |= crc4(m, i * 2 - 1);
    memcpy(m + i, body, blen); i += blen;
    m[i] = crc8(body, blen); i++;
    seqno ^= 1;
    dpcdW1(0x2003, 0x10);                                     /* clear a stale DOWN_REP_MSG_RDY */
    if (dpcdWriteLong(0x1000, m, i) < 0) { printf("  sideband write failed\n"); return -1; }
    int got = 0;
    for (int chunk = 0; chunk < 8; chunk++) {
        int ready = 0;
        for (int t = 0; t < 400 && !ready; t++) { int v = dpcdR1(0x2003); if (v >= 0 && (v & 0x10)) ready = 1; else { if (v >= 0 && (v & 0x20)) drainUpReq(); usleep(2500); } }
        if (!ready) { printf("  no sideband reply\n"); return -1; }
        uint8_t h[48];
        if (dpcdReadLong(0x1400, h, 16) < 0) return -1;
        const int lct = h[0] >> 4, hl = 1 + lct / 2 + 2, bl = h[1 + lct / 2] & 0x3F;
        const int somt = (h[hl - 1] >> 7) & 1, eomt = (h[hl - 1] >> 6) & 1;
        if (hl + bl > 48) return -1;
        if (hl + bl > 16) dpcdReadLong(0x1400 + 16, h + 16, hl + bl - 16);
        dpcdW1(0x2003, 0x10);
        (void)somt;
        const int payload = bl - 1;                           /* last byte is the body CRC */
        if (got + payload > repMax) return -1;
        memcpy(rep + got, h + hl, payload); got += payload;
        if (eomt) break;
    }
    return got;
}

struct Port { int num, input, type, ddps, mcs; };

static int linkAddress(struct Port *ports, int max) {
    uint8_t body[1] = { 0x01 }, r[256];
    int n = sideband(body, 1, 0, 0, r, sizeof r);
    if (n < 18) { printf("  LINK_ADDRESS failed (%d)\n", n); return -1; }
    if (r[0] & 0x80) { printf("  LINK_ADDRESS NAK reason %02x\n", n > 17 ? r[17] : 0); return -1; }
    int idx = 17, np = r[idx++] & 0xF, cnt = 0;
    printf("  hub GUID %02x%02x..%02x, %d ports\n", r[1], r[2], r[16], np);
    for (int p = 0; p < np && idx + 2 <= n; p++) {
        struct Port q = { r[idx] & 0xF, (r[idx] >> 7) & 1, (r[idx] >> 4) & 7, 0, 0 };
        idx++;
        q.mcs = (r[idx] >> 7) & 1; q.ddps = (r[idx] >> 6) & 1; idx++;
        if (!q.input) idx += 1 + 16 + 1;                      /* dpcd rev, peer GUID, sdp streams */
        static const char *types[] = { "none", "source/branch", "MST branch", "SST sink", "DP-legacy", "?", "?", "?" };
        printf("  port %d: %s, peer %s, %s\n", q.num, q.input ? "input" : "output", types[q.type], q.ddps ? "connected" : "empty");
        if (cnt < max) ports[cnt++] = q;
    }
    return cnt;
}

static int allocPayload(int port, int vcpi, int pbn) {
    uint8_t b[5] = { 0x11, (uint8_t)(port << 4), (uint8_t)(vcpi & 0x7F), (uint8_t)(pbn >> 8), (uint8_t)pbn }, r[32];
    int n = sideband(b, 5, 1, 0, r, sizeof r);
    if (n < 1 || (r[0] & 0x80)) { printf("  ALLOCATE_PAYLOAD port %d vcpi %d pbn %d: %s\n", port, vcpi, pbn, n < 1 ? "no reply" : "NAK"); return -1; }
    printf("  ALLOCATE_PAYLOAD port %d vcpi %d: allocated pbn %d\n", port, vcpi, n >= 5 ? (r[3] << 8) | r[4] : -1);
    return 0;
}

static int payloadTable(int vcpi, int start, int count) {
    uint8_t t[3] = { (uint8_t)vcpi, (uint8_t)start, (uint8_t)count };
    dpcdW1(0x2C0, 0x01);
    if (dpcdWriteLong(0x1C0, t, 3) < 0) return -1;
    for (int i = 0; i < 100; i++) { int v = dpcdR1(0x2C0); if (v >= 0 && (v & 1)) { dpcdW1(0x2C0, 0x01); return 0; } usleep(1000); }
    printf("  payload table update for vcpi %d not acknowledged\n", vcpi);
    return -1;
}

/* ---------- buffers ---------- */
static uint8_t *allocSlot(int slot, uint32_t bytes, uint32_t *ggtt) {
    uint64_t in[2] = { (uint64_t)slot, bytes }, out = 0; uint32_t n = 1;
    if (IOConnectCallScalarMethod(C, 6, in, 2, &out, &n)) { printf("alloc slot %d failed\n", slot); return NULL; }
    mach_vm_address_t a = 0; mach_vm_size_t sz = 0;
    if (IOConnectMapMemory64(C, slot, mach_task_self(), &a, &sz, kIOMapAnywhere)) return NULL;
    *ggtt = (uint32_t)out;
    return (uint8_t *)a;
}
static void pattern(uint8_t *p, uint32_t w, uint32_t h, uint32_t stride, int which) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(p + (size_t)y * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v;
            if (which == 0) v = ((x / 240 + y / 240) & 1) ? 0x2060FF : 0x102040;            /* blue checkerboard */
            else v = ((x / 240 + y / 240) & 1) ? 0xFF8020 : 0x402010;                       /* orange checkerboard */
            if ((y > h / 2 - 120 && y < h / 2 + 120) && x > w / 2 - 120 * (which + 1) && x < w / 2 + 120 * (which + 1)) v = 0xFFFFFF;
            row[x] = v;
        }
    }
}

/* Point a pipe's plane 1 at a native 3840x2160 surface with no scaler. */
static void planeSetup(uint32_t pipeOff, uint32_t ggtt, uint32_t bufStart, uint32_t bufEnd) {
    const uint32_t ps = (pipeOff / 0x1000) * 0x800;          /* scaler blocks are 0x800 apart per pipe */
    W(rPS1_CTRL + ps, 0); W(rPS1_WIN_POS + ps, 0); W(rPS1_WIN_SZ + ps, 0);
    W(rPIPESRC - 0x0 + pipeOff, (3839u << 16) | 2159u);
    W(rPLANE_STRIDE + pipeOff, 3840 * 4 / 64);
    W(rPLANE_SIZE + pipeOff, (2159u << 16) | 3839u);
    W(rPLANE_POS + pipeOff, 0); W(rPLANE_OFFSET + pipeOff, 0);
    W(rPLANE_BUFCFG + pipeOff, (bufEnd << 16) | bufStart);
    for (int i = 0; i < 8; i++) W(rPLANE_WM + pipeOff + 4 * i, R(rPLANE_WM + 4 * i));
    W(rPLANE_WMTRANS + pipeOff, R(rPLANE_WMTRANS));
    W(rPLANE_CTL + pipeOff, 0x84000008);                     /* enabled, linear XRGB8888, as the firmware's */
    W(rPLANE_SURF + pipeOff, ggtt);
}

static uint32_t ceilDiv(uint64_t a, uint64_t b) { return (uint32_t)((a + b - 1) / b); }

static void remoteRead(int port, uint32_t addr, int n) {
    uint8_t b[5] = { 0x20, (uint8_t)((port << 4) | ((addr >> 16) & 0xF)), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)n }, r[64];
    int got = sideband(b, 5, 0, 0, r, sizeof r);
    printf("  port %d DPCD %05x:", port, addr);
    if (got < 3 || (r[0] & 0x80)) { printf(" failed (%d)\n", got); return; }
    for (int i = 3; i < got; i++) printf(" %02x", r[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--peek")) {
        io_service_t s0 = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IGPUScanout"));
        if (!s0 || IOServiceOpen(s0, mach_task_self(), 0, &C)) return 1;
        seqno = argc > 2 ? atoi(argv[2]) : 0;
        for (int p = 1; p <= 2; p++) { remoteRead(p, 0x000, 16); remoteRead(p, 0x100, 8); remoteRead(p, 0x200, 8); remoteRead(p, 0x210, 8); }
        IOServiceClose(C);
        return 0;
    }
    int bppA = 24, bppC = 18, discoverOnly = 0, only = 0;   /* only: 0 both, 1 = stream A only, 2 = stream C only */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bpp-a") && i + 1 < argc) bppA = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bpp-c") && i + 1 < argc) bppC = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--discover")) discoverOnly = 1;
        else if (!strcmp(argv[i], "--only-a")) only = 1;
        else if (!strcmp(argv[i], "--only-c")) only = 2;
        else { fprintf(stderr, "usage: lgmst [--bpp-a 24|18] [--bpp-c 24|18] [--discover]\n"); return 2; }
    }
    if ((bppA != 24 && bppA != 18) || (bppC != 24 && bppC != 18)) { fprintf(stderr, "bpp must be 24 or 18\n"); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IGPUScanout"));
    if (!s || IOServiceOpen(s, mach_task_self(), 0, &C)) { fprintf(stderr, "cannot open IGPUScanout\n"); return 1; }
    signal(SIGINT, onSig); signal(SIGTERM, onSig); signal(SIGHUP, onSig);

    /* What the firmware set up. */
    const uint32_t funcA = R(rDDI_FUNC), linkM = R(rLINK_M1), linkN = R(rLINK_N1);
    if (!(funcA & (1u << 31)) || ((funcA >> 24) & 7) != 2) { printf("transcoder A is not the firmware's SST stream (%08x); stopping\n", funcA); return 1; }
    const uint64_t linkKHz = 810000;                          /* HBR3, from port clock register 0x64330 */
    const uint64_t pclkKHz = linkKHz * (linkM & 0xFFFFFF) / (linkN ? linkN : 1);
    printf("firmware stream: DDI_FUNC_CTL_A %08x, pixel clock %llu kHz\n", funcA, pclkKHz);

    /* Payload sizes: PBN = clock * bpp * 64 * 1.006 / (8 * 54 * 1000), 60 PBN per time slot on HBR3 x4. */
    const uint32_t pbnA = ceilDiv(pclkKHz * bppA * 64 * 1006, 8ULL * 54 * 1000 * 1000);
    const uint32_t pbnC = ceilDiv(pclkKHz * bppC * 64 * 1006, 8ULL * 54 * 1000 * 1000);
    const uint32_t slotsA = ceilDiv(pbnA, 60), slotsC = ceilDiv(pbnC, 60);
    printf("stream A %d bpp: PBN %u, %u slots; stream C %d bpp: PBN %u, %u slots; total %u of 63\n",
           bppA, pbnA, slotsA, bppC, pbnC, slotsC, slotsA + slotsC);
    if ((only ? (only == 1 ? slotsA : slotsC) : slotsA + slotsC) > 63) { printf("does not fit; use --bpp-c 18 (or both 18)\n"); return 1; }

    uint32_t ggA = 0, ggC = 0;
    uint8_t *bufA = NULL, *bufC = NULL;
    if (!discoverOnly) {
        bufA = allocSlot(1, 3840 * 4 * 2160, &ggA);
        bufC = allocSlot(2, 3840 * 4 * 2160, &ggC);
        if (!bufA || !bufC) { IOServiceClose(C); return 1; }
        pattern(bufA, 3840, 2160, 3840 * 4, 0);
        pattern(bufC, 3840, 2160, 3840 * 4, 1);
    }

    printf("1. stopping the SST stream\n");
    stopLink();
    printf("2. hub to MST mode\n");
    dpcdW1(0x111, 0x07);                                      /* MST_EN | UP_REQ_EN | UPSTREAM_IS_SRC, as DRM does */
    printf("3. training the link (MST)\n");
    int ok = train(1) == 0;
    struct Port ports[16]; int np = 0, outs[2] = { -1, -1 }, nouts = 0;
    if (ok) {
        printf("4. discovering the hub\n");
        uint8_t clr[1] = { 0x14 }, r[16];
        sideband(clr, 1, 1, 0, r, sizeof r);                  /* CLEAR_PAYLOAD_ID_TABLE */
        uint8_t t[3] = { 0, 0, 0x3F }; dpcdWriteLong(0x1C0, t, 3);
        for (int i = 0; i < 50; i++) { int v = dpcdR1(0x2C0); if (v >= 0 && (v & 1)) { dpcdW1(0x2C0, 1); break; } usleep(1000); }
        np = linkAddress(ports, 16);
        for (int i = 0; i < np; i++) if (!ports[i].input && ports[i].ddps && (ports[i].type == 3 || ports[i].type == 4) && nouts < 2) outs[nouts++] = ports[i].num;
        printf("  monitors on hub ports %d and %d\n", outs[0], outs[1]);
        for (int k = 0; k < nouts; k++) {
            uint8_t b[2] = { 0x10, (uint8_t)(outs[k] << 4) }, r[16]; int got = sideband(b, 2, 1, 0, r, sizeof r);
            if (got >= 6 && !(r[0] & 0x80)) printf("  port %d path resources: full %d PBN, available %d PBN\n", outs[k], (r[2] << 8) | r[3], (r[4] << 8) | r[5]);
        }
        if (nouts < 2) ok = 0;
    }
    if (ok && !discoverOnly) {
        const uint64_t dataN = 0x800000;
        const uint32_t mA = (uint32_t)(pclkKHz * bppA * dataN / (linkKHz * 4 * 8));
        const uint32_t mC = (uint32_t)(pclkKHz * bppC * dataN / (linkKHz * 4 * 8));
        int act = 0;
        if (only != 2) {
        printf("5. stream A (transcoder/pipe A -> port %d)\n", outs[0]);
        { uint8_t b[2] = { 0x24, (uint8_t)(outs[0] << 4) }, r[8]; sideband(b, 2, 1, 0, r, sizeof r); }
        payloadTable(1, 1, slotsA);
        W(rDATA_M1, ((slotsA - 1) << 25) | mA); W(rDATA_N1, (uint32_t)dataN);
        W(rMSA_MISC, bppA == 24 ? 0x21 : 0x01);
        RMW(rPIPEMISC, (7u << 5) | (1u << 4), bppA == 24 ? 0 : ((2u << 5) | (1u << 4)));
        const uint32_t funcMstA = (funcA & ~((7u << 24) | (7u << 20) | (3u << 10) | (1u << 8))) | (3u << 24) | (bppA == 24 ? 0 : (2u << 20));
        W(rDDI_FUNC, funcMstA);
        W(rDP_TP_STATUS, 1u << 24);                           /* clear ACT_SENT */
        W(rDDI_FUNC, funcMstA | (1u << 8));                   /* VC payload allocation -> ACT */
        act = 0; for (int i = 0; i < 20 && !act; i++) { act = (R(rDP_TP_STATUS) >> 24) & 1; usleep(1000); }
        printf("  ACT sent: %s, sink handled: %s\n", act ? "yes" : "NO", (dpcdR1(0x2C0) & 2) ? "yes" : "no");
        allocPayload(outs[0], 1, pbnA);
        planeSetup(0x0000, ggA, 0, 2047);                     /* pipe A: DBUF S1+S2 */
        W(rTRANSCONF, 1u << 31);
        }

        if (only != 1) {
        const uint32_t startC = only == 2 ? 1 : 1 + slotsA;
        printf("6. stream C (transcoder/pipe C -> port %d)\n", outs[1]);
        { uint8_t b[2] = { 0x24, (uint8_t)(outs[1] << 4) }, r[8]; sideband(b, 2, 1, 0, r, sizeof r); }
        for (uint32_t o = 0; o <= 0x14; o += 4) W(rHTOTAL + TRANS_C + o, R(rHTOTAL + o));
        W(rVSYNCSHIFT + TRANS_C, R(rVSYNCSHIFT));
        W(rCLK_SEL_C, R(rCLK_SEL_A));
        payloadTable(2, startC, slotsC);
        W(rDATA_M1 + TRANS_C, ((slotsC - 1) << 25) | mC); W(rDATA_N1 + TRANS_C, (uint32_t)dataN);
        W(rLINK_M1 + TRANS_C, linkM); W(rLINK_N1 + TRANS_C, linkN);
        W(rMSA_MISC + TRANS_C, bppC == 24 ? 0x21 : 0x01);
        W(rPIPEMISC + TRANS_C, (R(rPIPEMISC) & ~((7u << 5) | (1u << 4))) | (bppC == 24 ? 0 : ((2u << 5) | (1u << 4))));
        /* With A not streaming, C is the MST master itself (transport select C = 2). */
        const uint32_t funcMstBase = (funcA & ~((7u << 24) | (7u << 20) | (3u << 10) | (1u << 8))) | (3u << 24);
        const uint32_t funcMstC = funcMstBase | (bppC == 24 ? 0 : (2u << 20)) | ((only == 2 ? 2u : 0u) << 10);
        W(rDDI_FUNC + TRANS_C, funcMstC);
        W(rDP_TP_STATUS, 1u << 24);
        W(rDDI_FUNC + TRANS_C, funcMstC | (1u << 8));
        act = 0; for (int i = 0; i < 20 && !act; i++) { act = (R(rDP_TP_STATUS) >> 24) & 1; usleep(1000); }
        printf("  ACT sent: %s, sink handled: %s\n", act ? "yes" : "NO", (dpcdR1(0x2C0) & 2) ? "yes" : "no");
        allocPayload(outs[1], 2, pbnC);
        planeSetup(0x2000, ggC, 2048, 4095);                  /* pipe C: DBUF S3+S4 */
        W(rTRANSCONF + TRANS_C, 1u << 31);
        }
        for (int k = 0; k < 10; k++) { drainUpReq(); usleep(300000); }
        for (int p = 0; p < nouts; p++) { remoteRead(outs[p], 0x100, 8); remoteRead(outs[p], 0x200, 8); }
        waitFrames(0x0000, 3);
        printf("pipe A TRANSCONF %08x frame %u | pipe C TRANSCONF %08x frame %u | DP_TP_STATUS %08x\n",
               R(rTRANSCONF), R(rFRMCOUNT), R(rTRANSCONF + TRANS_C), R(rFRMCOUNT + TRANS_C), R(rDP_TP_STATUS));
        uint8_t st[6]; dpcdReadLong(0x200, st, 6);
        printf("sink: count %02x, lanes %02x %02x align %02x | payload table: ", st[0], st[2], st[3], st[4]);
        uint8_t pt[8]; dpcdReadLong(0x2C0, pt, 8); for (int i = 0; i < 8; i++) printf("%02x ", pt[i]); printf("\n");
        printf("\nEXPECTED: one LG blue checkerboard (small white box), the other orange (wider white box).\n");
        printf("Ctrl-C (or close stdin) to restore the firmware picture.\n");
        char line[256];
        while (!gStop && fgets(line, sizeof line, stdin)) {
            char *t[12]; int nt = 0;
            for (char *p = strtok(line, " \t\r\n"); p && nt < 12; p = strtok(NULL, " \t\r\n")) t[nt++] = p;
            if (!nt) continue;
            if (!strcmp(t[0], "peek")) { for (int p = 1; p <= 2; p++) { remoteRead(p, 0x100, 8); remoteRead(p, 0x200, 8); remoteRead(p, 0x210, 8); } }
            else if (!strcmp(t[0], "rr") && nt >= 4) remoteRead(atoi(t[1]), (uint32_t)strtoul(t[2], 0, 16), atoi(t[3]));
            else if (!strcmp(t[0], "rw") && nt >= 4) {      /* rw <port> <addr> <byte> ... : REMOTE_DPCD_WRITE */
                int port = atoi(t[1]); uint32_t a = (uint32_t)strtoul(t[2], 0, 16);
                uint8_t b[24] = { 0x21, (uint8_t)((port << 4) | ((a >> 16) & 0xF)), (uint8_t)(a >> 8), (uint8_t)a, 0 }; int n = 0;
                for (int i = 3; i < nt && n < 16; i++) b[5 + n++] = (uint8_t)strtoul(t[i], 0, 16);
                b[4] = (uint8_t)n; uint8_t r[32];
                int got = sideband(b, 5 + n, 0, 0, r, sizeof r);
                printf("  REMOTE_DPCD_WRITE port %d %05x x%d: %s\n", port, a, n, got < 1 ? "no reply" : (r[0] & 0x80) ? "NAK" : "ok");
            }
            else if (!strcmp(t[0], "phy") && nt >= 3) {     /* phy <port> up|down */
                uint8_t b[2] = { (uint8_t)(!strcmp(t[2], "up") ? 0x24 : 0x25), (uint8_t)(atoi(t[1]) << 4) }, r[16];
                int got = sideband(b, 2, 1, 0, r, sizeof r);
                printf("  POWER_%s_PHY port %s: %s\n", t[2], t[1], got < 1 ? "no reply" : (r[0] & 0x80) ? "NAK" : "ok");
            }
            else if (!strcmp(t[0], "w") && nt >= 3) { uint32_t o = (uint32_t)strtoul(t[1], 0, 16), v = (uint32_t)strtoul(t[2], 0, 16); W(o, v); printf("  w %06x <- %08x reads %08x\n", o, v, R(o)); }
            else if (!strcmp(t[0], "r") && nt >= 2) { uint32_t o = (uint32_t)strtoul(t[1], 0, 16); int n = nt > 2 ? atoi(t[2]) : 1; for (int i = 0; i < n; i++) printf("  %06x = %08x\n", o + 4 * i, R(o + 4 * i)); }
            else if (!strcmp(t[0], "dpcd") && nt >= 2) { uint32_t a = (uint32_t)strtoul(t[1], 0, 16); uint8_t b[16]; int n = dpcdRead(a, b, nt > 2 ? atoi(t[2]) : 1); printf("  dpcd %05x:", a); for (int i = 0; i < n; i++) printf(" %02x", b[i]); printf("\n"); }
            else if (!strcmp(t[0], "fill") && nt >= 3) { uint8_t *p = atoi(t[1]) == 1 ? bufA : bufC; uint32_t v = (uint32_t)strtoul(t[2], 0, 16);
                for (size_t i = 0; i < (size_t)3840 * 2160; i++) ((uint32_t *)p)[i] = v; printf("  buffer %s filled %06x\n", t[1], v); }
            else if (!strcmp(t[0], "enum") && nt >= 2) {
                uint8_t b[2] = { 0x10, (uint8_t)(atoi(t[1]) << 4) }, r[16]; int got = sideband(b, 2, 1, 0, r, sizeof r);
                if (got >= 6 && !(r[0] & 0x80)) printf("  ENUM_PATH_RESOURCES port %s: fec %d full_pbn %d avail_pbn %d\n", t[1], r[1] & 1, (r[2] << 8) | r[3], (r[4] << 8) | r[5]);
                else printf("  ENUM_PATH_RESOURCES port %s failed (%d)\n", t[1], got);
            }
            else if (!strcmp(t[0], "alloc") && nt >= 4) allocPayload(atoi(t[1]), atoi(t[2]), atoi(t[3]));
            else if (!strcmp(t[0], "quit")) break;
            else printf("  ? %s\n", t[0]);
            fflush(stdout);
        }
    }

    printf("restoring: streams off, hub back to SST, SST retrain\n");
    if (ok && !discoverOnly) {
        allocPayload(outs[1], 2, 0); allocPayload(outs[0], 1, 0);
    }
    stopLink();
    uint8_t t[3] = { 0, 0, 0x3F }; dpcdWriteLong(0x1C0, t, 3); dpcdW1(0x2C0, 1);
    dpcdW1(0x111, 0x00);
    IOConnectCallScalarMethod(C, 7, NULL, 0, NULL, NULL);    /* kext: registers back to the firmware's values, buffers freed */
    usleep(50000);
    train(0);                                                 /* then retrain SST under the firmware's running stream */
    printf("after undo: TRANSCONF_A %08x DDI_FUNC_A %08x DP_TP_CTL %08x, sink lanes ", R(rTRANSCONF), R(rDDI_FUNC), R(rDP_TP_CTL));
    uint8_t st[3]; dpcdReadLong(0x202, st, 3); printf("%02x %02x %02x\n", st[0], st[1], st[2]);
    IOServiceClose(C);
    return 0;
}

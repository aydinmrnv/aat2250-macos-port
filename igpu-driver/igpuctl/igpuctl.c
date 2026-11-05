/*
 * igpuctl - development console for IGPUScanout 0.0.3 (iGPU 7D67 display bring-up).
 *
 * Reads commands from stdin, one per line, and keeps the kext connection open, so register writes made through it
 * stay in effect until it exits; then the kext undoes them (newest first) and frees any buffers it allocated.
 *
 *   r <off> [n]            read n dwords (hex offsets)
 *   w <off> <val>          write a display register (logged for undo)
 *   dpcd <addr> [n]        native AUX read on TC1 (n <= 16), retries DEFER
 *   dpcdw <addr> <b0> ...  native AUX write (<= 16 bytes)
 *   probe                  read the DPCD areas that matter for MST and print them
 *   alloc <slot> <bytes>   allocate scanout buffer slot 1..3, print its GGTT offset
 *   fill <slot> <rgb>      fill a slot with a colour (hex 0xRRGGBB)
 *   bars <slot> <w> <h> <stride>
 *   undo                   undo everything now
 *   sleep <ms>
 *   q                      quit (also undoes)
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static io_connect_t C;
static mach_vm_address_t slotAddr[4];
static mach_vm_size_t slotSize[4];

static int rr(uint32_t off, uint32_t *v) {
    uint64_t in = off, out = 0; uint32_t n = 1;
    kern_return_t kr = IOConnectCallScalarMethod(C, 3, &in, 1, &out, &n);
    *v = (uint32_t)out; return kr;
}
static int ww(uint32_t off, uint32_t v) {
    uint64_t in[2] = { off, v };
    return IOConnectCallScalarMethod(C, 4, in, 2, NULL, NULL);
}

/* One raw AUX transfer; returns reply length or -1. */
static int auxRaw(const uint8_t *tx, int txLen, uint8_t *rx, uint32_t *status) {
    uint64_t in[5] = { 0, (uint64_t)txLen, 0, 0, 0 }, out[5] = {0}; uint32_t n = 5;
    for (int i = 0; i < txLen; i++) in[2 + i / 8] |= (uint64_t)tx[i] << ((i % 8) * 8);
    kern_return_t kr = IOConnectCallScalarMethod(C, 5, in, 5, out, &n);
    *status = (uint32_t)out[0];
    if (kr) return -1;
    int len = (int)out[1];
    for (int i = 0; i < len && i < 20; i++) rx[i] = (uint8_t)(out[2 + i / 8] >> ((i % 8) * 8));
    if (out[0] & ((1u << 28) | (1u << 25))) return -1;     /* timeout / receive error */
    return len;
}

/* Native AUX read/write with DEFER retries (DP 1.4: request 0x9 read, 0x8 write; reply high nibble 0 ACK, 1 NACK, 2 DEFER). */
int dpcdRead(uint32_t addr, uint8_t *buf, int len) {
    uint8_t tx[4] = { (uint8_t)(0x90 | ((addr >> 16) & 0xF)), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(len - 1) };
    for (int tries = 0; tries < 32; tries++) {
        uint8_t rx[20]; uint32_t st;
        int n = auxRaw(tx, 4, rx, &st);
        if (n < 1) { usleep(1000); continue; }
        const uint8_t rep = rx[0] >> 4;
        if (rep == 0x2) { usleep(500); continue; }          /* DEFER */
        if (rep != 0x0) return -2;                          /* NACK */
        memcpy(buf, rx + 1, n - 1);
        return n - 1;
    }
    return -1;
}
int dpcdWrite(uint32_t addr, const uint8_t *buf, int len) {
    uint8_t tx[20] = { (uint8_t)(0x80 | ((addr >> 16) & 0xF)), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(len - 1) };
    memcpy(tx + 4, buf, len);
    for (int tries = 0; tries < 32; tries++) {
        uint8_t rx[20]; uint32_t st;
        int n = auxRaw(tx, 4 + len, rx, &st);
        if (n < 1) { usleep(1000); continue; }
        const uint8_t rep = rx[0] >> 4;
        if (rep == 0x2) { usleep(500); continue; }
        if (rep != 0x0) return -2;
        return len;
    }
    return -1;
}

static void hexdump(uint32_t addr, const uint8_t *b, int n) {
    printf("  %05x:", addr);
    for (int i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}

static void dumpArea(const char *name, uint32_t addr, int len) {
    printf("%s\n", name);
    for (int off = 0; off < len; off += 16) {
        uint8_t b[16]; int n = dpcdRead(addr + off, b, len - off < 16 ? len - off : 16);
        if (n < 0) { printf("  %05x: read failed (%d)\n", addr + off, n); return; }
        hexdump(addr + off, b, n);
    }
}

static void probe(void) {
    dumpArea("receiver caps (0x000)", 0x000, 16);
    dumpArea("MSTM_CAP / misc (0x020)", 0x020, 16);
    dumpArea("DSC caps (0x060)", 0x060, 16);
    dumpArea("downstream ports (0x080)", 0x080, 16);
    dumpArea("link config (0x100)", 0x100, 16);
    dumpArea("MSTM_CTRL etc (0x110)", 0x110, 16);
    dumpArea("payload table (0x1c0)", 0x1C0, 16);
    dumpArea("sink status (0x200)", 0x200, 16);
    dumpArea("branch OUI / id (0x500)", 0x500, 16);
    dumpArea("extended caps (0x2200)", 0x2200, 16);
}

static int mapSlot(int s) {
    if (slotAddr[s]) return 0;
    kern_return_t kr = IOConnectMapMemory64(C, s, mach_task_self(), &slotAddr[s], &slotSize[s], kIOMapAnywhere);
    if (kr) { printf("map slot %d failed 0x%x\n", s, kr); return -1; }
    return 0;
}

int main(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IGPUScanout"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &C)) { fprintf(stderr, "cannot open IGPUScanout\n"); return 1; }
    IOObjectRelease(svc);
    setvbuf(stdout, NULL, _IOLBF, 0);
    char line[512];
    printf("igpuctl ready\n");
    while (fgets(line, sizeof line, stdin)) {
        char *tok[24]; int nt = 0;
        for (char *p = strtok(line, " \t\r\n"); p && nt < 24; p = strtok(NULL, " \t\r\n")) tok[nt++] = p;
        if (!nt || tok[0][0] == '#') continue;
        const char *c = tok[0];
        if (!strcmp(c, "q")) break;
        else if (!strcmp(c, "r") && nt >= 2) {
            uint32_t off = (uint32_t)strtoul(tok[1], 0, 16), n = nt > 2 ? (uint32_t)strtoul(tok[2], 0, 0) : 1;
            for (uint32_t i = 0; i < n; i++) { uint32_t v; int kr = rr(off + 4 * i, &v); printf("%06x = %08x%s\n", off + 4 * i, v, kr ? " (err)" : ""); }
        } else if (!strcmp(c, "w") && nt >= 3) {
            uint32_t off = (uint32_t)strtoul(tok[1], 0, 16), v = (uint32_t)strtoul(tok[2], 0, 16);
            int kr = ww(off, v); uint32_t back = 0; rr(off, &back);
            printf("w %06x <- %08x : %s, reads %08x\n", off, v, kr ? "REFUSED" : "ok", back);
        } else if (!strcmp(c, "dpcd") && nt >= 2) {
            uint32_t a = (uint32_t)strtoul(tok[1], 0, 16); int n = nt > 2 ? atoi(tok[2]) : 1; if (n > 16) n = 16;
            uint8_t b[16]; int got = dpcdRead(a, b, n);
            if (got < 0) printf("dpcd %05x: failed %d\n", a, got); else hexdump(a, b, got);
        } else if (!strcmp(c, "dpcdw") && nt >= 3) {
            uint32_t a = (uint32_t)strtoul(tok[1], 0, 16); uint8_t b[16]; int n = 0;
            for (int i = 2; i < nt && n < 16; i++) b[n++] = (uint8_t)strtoul(tok[i], 0, 16);
            printf("dpcdw %05x (%d bytes): %d\n", a, n, dpcdWrite(a, b, n));
        } else if (!strcmp(c, "probe")) probe();
        else if (!strcmp(c, "alloc") && nt >= 3) {
            uint64_t in[2] = { strtoul(tok[1], 0, 0), strtoul(tok[2], 0, 0) }, out = 0; uint32_t n = 1;
            kern_return_t kr = IOConnectCallScalarMethod(C, 6, in, 2, &out, &n);
            printf("alloc slot %llu: %s ggtt %08llx\n", in[0], kr ? "FAILED" : "ok", out);
            if (!kr) mapSlot((int)in[0]);
        } else if (!strcmp(c, "fill") && nt >= 3) {
            int s = atoi(tok[1]); uint32_t rgb = (uint32_t)strtoul(tok[2], 0, 16);
            if (s < 1 || s > 3 || mapSlot(s)) continue;
            uint32_t *p = (uint32_t *)slotAddr[s];
            for (mach_vm_size_t i = 0; i < slotSize[s] / 4; i++) p[i] = rgb;
            printf("slot %d filled %06x\n", s, rgb);
        } else if (!strcmp(c, "bars") && nt >= 5) {
            int s = atoi(tok[1]); uint32_t W = atoi(tok[2]), H = atoi(tok[3]), S = atoi(tok[4]);
            if (s < 1 || s > 3 || mapSlot(s) || (mach_vm_size_t)S * H > slotSize[s]) continue;
            static const uint32_t bars[8] = { 0x0000FF, 0xFF0000, 0x00FF00, 0xFF00FF, 0x00FFFF, 0xFFFF00, 0xFFFFFF, 0x000000 };
            for (uint32_t y = 0; y < H; y++) { uint32_t *row = (uint32_t *)(slotAddr[s] + (size_t)y * S);
                for (uint32_t x = 0; x < W; x++) row[x] = bars[x * 8 / W]; }
            printf("slot %d: reversed bars %ux%u\n", s, W, H);
        } else if (!strcmp(c, "undo")) { IOConnectCallScalarMethod(C, 7, NULL, 0, NULL, NULL); printf("undone\n"); }
        else if (!strcmp(c, "sleep") && nt >= 2) usleep((useconds_t)atoi(tok[1]) * 1000);
        else printf("? %s\n", c);
    }
    IOServiceClose(C);
    printf("closed (kext undid all writes)\n");
    return 0;
}

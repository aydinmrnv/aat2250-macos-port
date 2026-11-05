// nvrmcu-selftest: the driver layer on its own, no CUDA runtime in between. Every check prints PASS or FAIL and the
// last line says how many of each. Safe to run with the desktop up: it opens an ordinary RM client, as an app would.
#include "tinynv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void nvrmcu_stats(uint64_t *launches, uint64_t *batches);

static int pass, fail;
static void check(int ok, const char *what) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) printf("      last error: %s\n", tinynv_last_error());
  ok ? pass++ : fail++;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("nvrmcu self-test (driver build %s)\n", tinynv_build_id());
  tinynv_status_t st = tinynv_init();
  check(st == TINYNV_OK, "open the GPU through the NVIDIA RM");
  if (st != TINYNV_OK) { printf("\nRESULT: %d passed, %d failed (no GPU, nothing else can run)\n", pass, fail); return 1; }

  tinynv_device_t d;
  check(tinynv_device_get(&d, 0) == TINYNV_OK, "device 0");
  tinynv_device_props_t p;
  check(tinynv_device_props(d, &p) == TINYNV_OK, "device properties");
  printf("      %s, compute capability %d.%d, %d SMs, %.1f GB\n", p.name, p.cc_major, p.cc_minor, p.sm_count, p.total_mem / 1e9);
  uint64_t fr = 0, tot = 0;
  if (tinynv_mem_info(d, &fr, &tot) == TINYNV_OK) printf("      video memory free %.1f GB of %.1f GB\n", fr / 1e9, tot / 1e9);

  tinynv_stream_t s;
  tinynv_stream_create(d, &s);

  const size_t n = 64u << 20;
  uint8_t *h = malloc(n), *r = malloc(n);
  for (size_t i = 0; i < n; i++) h[i] = (uint8_t)(i * 2654435761u >> 13);
  tinynv_devptr_t a = 0, b = 0;
  check(tinynv_malloc(d, n, &a) == TINYNV_OK && a, "allocate 64 MB of video memory (A)");
  check(tinynv_malloc(d, n, &b) == TINYNV_OK && b, "allocate 64 MB of video memory (B)");
  printf("      A at %#llx, B at %#llx\n", (unsigned long long)a, (unsigned long long)b);

  // smallest first: one dword, then an odd size
  uint32_t one = 0xC0FFEE11, back = 0;
  int ok = tinynv_memcpy_htod(s, a, &one, 4) == TINYNV_OK && tinynv_memcpy_dtoh(s, &back, a, 4) == TINYNV_OK;
  check(ok && back == one, "4 bytes up and back (the copy kernel runs on the GPU)");
  if (!ok || back != one) printf("      read back 0x%08x\n", back);

  memset(r, 0, 1001);
  ok = tinynv_memcpy_htod(s, a + 3, h, 1001) == TINYNV_OK && tinynv_memcpy_dtoh(s, r, a + 3, 1001) == TINYNV_OK;
  check(ok && !memcmp(h, r, 1001), "1001 bytes at an odd address");

  double t0 = now();
  ok = tinynv_memcpy_htod(s, a, h, n) == TINYNV_OK && tinynv_stream_sync(s) == TINYNV_OK;
  double up = now() - t0;
  memset(r, 0, n);
  t0 = now();
  ok = ok && tinynv_memcpy_dtoh(s, r, a, n) == TINYNV_OK;
  double down = now() - t0;
  check(ok && !memcmp(h, r, n), "64 MB up and back, byte for byte");
  printf("      upload %.2f GB/s, download %.2f GB/s (v0.1 copies through a staging buffer)\n", n / up / 1e9, n / down / 1e9);

  ok = tinynv_memset(s, b, 0x5a, n) == TINYNV_OK && tinynv_memcpy_dtoh(s, r, b, n) == TINYNV_OK;
  int all = ok;
  for (size_t i = 0; all && i < n; i++) all = r[i] == 0x5a;
  check(all, "memset 64 MB to 0x5a");

  ok = tinynv_memcpy_dtod(s, b, a, n) == TINYNV_OK && tinynv_memcpy_dtoh(s, r, b, n) == TINYNV_OK;
  check(ok && !memcmp(h, r, n), "64 MB GPU to GPU");

  // a big allocation, the size of a mid-sized model
  tinynv_devptr_t big = 0;
  const size_t bn = 8ull << 30;
  ok = tinynv_malloc(d, bn, &big) == TINYNV_OK;
  check(ok, "allocate 8 GB of video memory");
  if (ok) {
    ok = tinynv_memcpy_htod(s, big + bn - n, h, n) == TINYNV_OK && tinynv_memcpy_dtoh(s, r, big + bn - n, n) == TINYNV_OK;
    check(ok && !memcmp(h, r, n), "the last 64 MB of it, up and back");
    check(tinynv_free(d, big) == TINYNV_OK, "free 8 GB");
  }

  tinynv_event_t e;
  tinynv_event_create(d, &e);
  tinynv_memset(s, a, 0, n);
  tinynv_event_record(e, s);
  check(tinynv_event_sync(e) == TINYNV_OK, "event record and wait");

  check(tinynv_free(d, a) == TINYNV_OK && tinynv_free(d, b) == TINYNV_OK, "free A and B");
  uint64_t launches = 0, batches = 0;
  nvrmcu_stats(&launches, &batches);
  printf("      %llu kernel launches in %llu batches\n", (unsigned long long)launches, (unsigned long long)batches);
  printf("\nRESULT: %d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}

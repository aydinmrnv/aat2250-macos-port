// nvrmcu.c - macuda's driver interface (tinynv.h) implemented on NVIDIA's resource manager, as NullMoth's NVRM kext
// exposes it on an x86 Mac (a Hackintosh with the card on PCIe), instead of on tinygrad's TinyGPU dext.
//
// The difference that matters: macuda boots the GPU itself (GSP firmware, page tables, rings) because on an Apple
// Silicon eGPU nothing else owns the card. Here the kext already owns it and is driving the desktop with it, so this
// library is an ordinary RM client - the same kind of client NullMoth's Vulkan driver is - and asks the RM for
// everything: memory, an address space, a channel, a compute object. The display keeps running alongside.
//
// The RM sequence (which objects, which parameters) follows NullMoth's Vulkan backend, which is proven on this kext.
// The command encoders, launch descriptor (QMD), cubin parser and image layout are macuda's, unchanged.
//
// Version 0.1 is correctness first: one channel, every batch ends with a wait-for-idle semaphore release, and all
// copies are done by a tiny copy kernel on the compute engine through a pinned system-memory staging buffer, so no
// assumption is made about how much video memory the CPU can see through BAR1.
#include "tinynv.h"
#include "internal.h"
#include "cubin.h"
#include "qmd.h"
#include "submit.h"
#include "nvrm_io.h"

#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nvtypes.h"
#include "nvmisc.h"
#include "nvos.h"
#include "nvstatus.h"
#include "nv-ioctl.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "nvgputypes.h"
#include "class/cl0000.h"
#include "class/cl0002.h"
#include "class/cl003e.h"
#include "class/cl0040.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl50a0.h"
#include "class/cl9067.h"
#include "class/cl906f.h"
#include "class/cl90f1.h"
#include "class/cla06c.h"
#include "class/clc361.h"
#include "class/clc461.h"
#include "class/clc56f.h"
#include "class/clc96f.h"
#include "class/clca6f.h"
#include "class/clcdc0.h"
#include "class/clcec0.h"
#include "alloc/alloc_channel.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl0080/ctrl0080gr.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl2080/ctrl2080gr.h"
#include "ctrl/ctrla06f/ctrla06fgpfifo.h"
#include "ctrl/ctrlc36f.h"

// macuda's submit.c writes the doorbell through this on its own hardware path, which this library never takes.
void tinynv_wr32(void *d, uint64_t off, uint32_t v) { (void)d; (void)off; (void)v; abort(); }

extern unsigned char nvrmcu_copy1d_cubin[];
extern unsigned int nvrmcu_copy1d_cubin_len;

#define RING_BYTES   (64ull << 20)   // command batches and launch descriptors
#define STAGE_BYTES  (64ull << 20)   // host <-> device staging
#define GPFIFO_ENTRIES 0x8000u
#define WAIT_TIMEOUT_S 20.0
#define SHARED_WINDOW 0x729400000000ull
#define LOCAL_WINDOW  0x729300000000ull

typedef struct {
  NvHandle hphys, hvirt;
  uint64_t va, size;
  uint8_t *cpu;
  nvrm_conn_t mc;
  int sys;
} mem_t;

struct tinynv_module {
  tinynv_device_t dev;
  void *image;
  size_t len;
  tinynv_cubin_t cubin;
  mem_t mem;
  tinynv_image_t layout;
  int loaded;
};

struct tinynv_kernel {
  tinynv_module_t mod;
  const tinynv_kernel_desc_t *desc;
  tinynv_image_t layout;
  int has_layout;
};

struct tinynv_stream { tinynv_device_t dev; };
struct tinynv_event { tinynv_device_t dev; uint64_t value; };

typedef struct { uint64_t va; mem_t m; } alloc_t;

struct tinynv_device {
  int up, dead;
  nvrm_conn_t ctl, devc, umc;
  NvHandle hClient, hDevice, hSubdevice, hVaSpace, hUsermode, hTsg, hCtxShare, hChannel, hCompute, hCtxDma;
  uint32_t cls_gpfifo, cls_compute, cls_usermode;
  volatile uint32_t *usermode;
  mem_t notifier, userd, gpfifo, sem, ring, stage, slm;
  uint32_t token, gp_put;
  uint64_t seq;            // last value a submitted batch will release
  uint64_t ring_off, stage_off;
  uint32_t sm_version, num_gpcs, tpc_per_gpc, sm_per_tpc, max_warps, sm_count;
  uint64_t vram_total, vram_free_at_open, live_bytes;
  char name[128];
  uint32_t slm_per_thread;
  tinynv_module_t copy_mod;
  tinynv_kernel_t copy_k;
  alloc_t *allocs;
  int nallocs, cap_allocs;
  uint64_t launches, batches;
};

static struct tinynv_device g_dev;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_init_done;
static tinynv_status_t g_init_status = TINYNV_ERR_NODEV;

#define TRACE(...) do { if (nvrm_trace()) fprintf(stderr, "nvrmcu: " __VA_ARGS__); } while (0)

// *** RM calls

static NV_STATUS rm_alloc(tinynv_device_t d, NvHandle parent, NvHandle *out, NvU32 cls, void *params) {
  NVOS21_PARAMETERS p = {.hRoot = d->hClient, .hObjectParent = parent, .hObjectNew = 0, .hClass = cls, .pAllocParms = params};
  if (nvrm_esc(&d->ctl, NV_ESC_RM_ALLOC, &p, sizeof(p))) return NV_ERR_GENERIC;
  TRACE("alloc class 0x%04x under 0x%08x -> 0x%08x status 0x%x\n", cls, parent, p.hObjectNew, p.status);
  if (p.status == NV_OK) *out = p.hObjectNew;
  return p.status;
}

static NV_STATUS rm_control(tinynv_device_t d, NvHandle obj, NvU32 cmd, void *params, NvU32 size) {
  NVOS54_PARAMETERS p = {.hClient = d->hClient, .hObject = obj, .cmd = cmd, .params = params, .paramsSize = size};
  if (nvrm_esc(&d->ctl, NV_ESC_RM_CONTROL, &p, sizeof(p))) return NV_ERR_GENERIC;
  TRACE("control 0x%08x on 0x%08x status 0x%x\n", cmd, obj, p.status);
  return p.status;
}

static void rm_free(tinynv_device_t d, NvHandle h) {
  if (!h) return;
  NVOS00_PARAMETERS p = {.hRoot = d->hClient, .hObjectParent = 0, .hObjectOld = h};
  nvrm_esc(&d->ctl, NV_ESC_RM_FREE, &p, sizeof(p));
}

static int rm_fail(const char *what, NV_STATUS st) {
  return tinynv_fail("%s: NVIDIA RM status 0x%x", what, st);
}

// *** memory: physical (video or system), a virtual range in our address space, the GPU mapping, and optionally a
// CPU mapping.

static int mem_alloc(tinynv_device_t d, uint64_t size, int sys, int cpu, mem_t *m) {
  memset(m, 0, sizeof(*m));
  m->mc.fd = -1;
  const uint64_t page = sys ? 0x1000 : 0x10000;
  size = (size + page - 1) & ~(page - 1);
  m->size = size;
  m->sys = sys;

  NV_MEMORY_ALLOCATION_PARAMS pp = {
    .owner = d->hClient, .type = NVOS32_TYPE_IMAGE, .flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
    .size = size, .alignment = page,
  };
  if (sys) {
    pp.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB) | DRF_DEF(OS32, _ATTR, _LOCATION, _PCI) |
              DRF_DEF(OS32, _ATTR, _COHERENCY, _CACHED);
  } else {
    pp.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _BIG) | DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM) |
              DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED) | DRF_DEF(OS32, _ATTR, _PHYSICALITY, _ALLOW_NONCONTIGUOUS);
    pp.flags |= NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM;
  }
  NV_STATUS st = rm_alloc(d, d->hDevice, &m->hphys, sys ? NV01_MEMORY_SYSTEM : NV01_MEMORY_LOCAL_USER, &pp);
  if (st == NV_ERR_NO_MEMORY || st == NV_ERR_INSUFFICIENT_RESOURCES) {
    tinynv_fail("out of %s memory allocating %llu bytes", sys ? "system" : "video", (unsigned long long)size);
    return -2;
  }
  if (st) return rm_fail(sys ? "allocating system memory" : "allocating video memory", st);

  NV_MEMORY_ALLOCATION_PARAMS vp = {
    .owner = d->hClient, .type = NVOS32_TYPE_IMAGE, .flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
    .size = size, .alignment = page, .hVASpace = d->hVaSpace,
  };
  st = rm_alloc(d, d->hDevice, &m->hvirt, NV50_MEMORY_VIRTUAL, &vp);
  if (st) { rm_free(d, m->hphys); return rm_fail("reserving GPU virtual addresses", st); }
  m->va = vp.offset;

  NVOS46_PARAMETERS mp = {
    .hClient = d->hClient, .hDevice = d->hDevice, .hDma = m->hvirt, .hMemory = m->hphys, .offset = 0, .length = size,
    .flags = DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL) |
             (sys ? DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE) : DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE)),
    .dmaOffset = 0,
  };
  if (nvrm_esc(&d->ctl, NV_ESC_RM_MAP_MEMORY_DMA, &mp, sizeof(mp)) || mp.status) {
    rm_free(d, m->hvirt); rm_free(d, m->hphys);
    return rm_fail("mapping memory into the GPU address space", mp.status ? mp.status : NV_ERR_GENERIC);
  }
  if (mp.dmaOffset != m->va && mp.dmaOffset != 0)
    TRACE("note: map returned dmaOffset %#llx for a range reserved at %#llx\n", (unsigned long long)mp.dmaOffset,
          (unsigned long long)m->va);

  if (cpu) {
    if (nvrm_open(&m->mc, sys)) { rm_free(d, m->hvirt); rm_free(d, m->hphys); return tinynv_fail("opening a mapping connection"); }
    nv_ioctl_nvos33_parameters_with_fd cp = {
      .params = {.hClient = d->hClient, .hDevice = d->hSubdevice, .hMemory = m->hphys, .offset = 0, .length = size,
                 // as NullMoth's Vulkan driver maps them: system memory directly (bit 15), video memory with default
                 // (uncached) caching - the only video memory mapped here is the channel's control page, which the
                 // CPU polls and must see the GPU's writes to.
                 .pLinearAddress = 0, .flags = sys ? (1u << 15) : 0},
      .fd = m->mc.fd,
    };
    if (nvrm_esc(&d->ctl, NV_ESC_RM_MAP_MEMORY, &cp, sizeof(cp)) || cp.params.status) {
      nvrm_close(&m->mc); rm_free(d, m->hvirt); rm_free(d, m->hphys);
      return rm_fail("mapping memory for the CPU", cp.params.status ? cp.params.status : NV_ERR_GENERIC);
    }
    size_t got = 0;
    m->cpu = nvrm_map(&m->mc, &got, 0);
    if (!m->cpu || got < size) {
      if (m->cpu) nvrm_unmap(&m->mc, m->cpu);
      nvrm_close(&m->mc); rm_free(d, m->hvirt); rm_free(d, m->hphys);
      return tinynv_fail("the CPU mapping came back %zu bytes for %llu", got, (unsigned long long)size);
    }
  }
  TRACE("mem %s %llu bytes at gpu %#llx cpu %p\n", sys ? "sys" : "vid", (unsigned long long)size,
        (unsigned long long)m->va, (void *)m->cpu);
  return 0;
}

static void mem_free(tinynv_device_t d, mem_t *m) {
  if (m->cpu) {
    nvrm_unmap(&m->mc, m->cpu);
    NVOS34_PARAMETERS up = {.hClient = d->hClient, .hDevice = d->hSubdevice, .hMemory = m->hphys, .pLinearAddress = 0, .flags = 0};
    (void)up;   // the mapping context dies with its connection
    nvrm_close(&m->mc);
  }
  if (m->hvirt) {
    NVOS47_PARAMETERS u = {.hClient = d->hClient, .hDevice = d->hDevice, .hDma = m->hvirt, .hMemory = m->hphys, .flags = 0, .dmaOffset = m->va};
    nvrm_esc(&d->ctl, NV_ESC_RM_UNMAP_MEMORY_DMA, &u, sizeof(u));
    rm_free(d, m->hvirt);
  }
  rm_free(d, m->hphys);
  memset(m, 0, sizeof(*m));
}

// *** the channel: completion, submission

static uint64_t sem_done(tinynv_device_t d) { return __atomic_load_n((volatile uint64_t *)d->sem.cpu, __ATOMIC_ACQUIRE); }

static int channel_error(tinynv_device_t d) {
  NvNotification *n = (NvNotification *)d->notifier.cpu;
  const NvNotification *e = &n[NV_CHANNELGPFIFO_NOTIFICATION_TYPE_ERROR];
  if (e->status) {
    d->dead = 1;
    return tinynv_fail("the GPU reported an error on the compute channel: status 0x%x info32 0x%x info16 0x%x "
                       "(a fault in a kernel, or a bad command)", e->status, e->info32, e->info16);
  }
  return 0;
}

static int wait_seq(tinynv_device_t d, uint64_t want) {
  if (d->dead) return tinynv_fail("the channel has failed earlier; restart the program");
  double t0 = tinynv_now_s();
  unsigned spins = 0;
  while (sem_done(d) < want) {
    if (channel_error(d)) return -1;
    if (++spins > 2000) {
      usleep(spins > 20000 ? 200 : 10);
      if (tinynv_now_s() - t0 > WAIT_TIMEOUT_S) {
        AmpereAControlGPFifo *u = (AmpereAControlGPFifo *)d->userd.cpu;
        d->dead = 1;
        return tinynv_fail("the GPU did not finish within %.0f s: completed %llu of %llu, GPGet %u GPPut %u", WAIT_TIMEOUT_S,
                           (unsigned long long)sem_done(d), (unsigned long long)want, u->GPGet, u->GPPut);
      }
    } else sched_yield();
  }
  return channel_error(d);
}

static int sync_all(tinynv_device_t d) { return wait_seq(d, d->seq); }

// Room for a batch or a descriptor in the ring. On wrap, everything earlier has to be finished first.
static uint8_t *ring_take(tinynv_device_t d, uint64_t n, uint64_t align, uint64_t *va) {
  uint64_t off = (d->ring_off + align - 1) & ~(align - 1);
  if (off + n > d->ring.size) {
    if (sync_all(d)) return NULL;
    off = 0;
  }
  d->ring_off = off + n;
  *va = d->ring.va + off;
  return d->ring.cpu + off;
}

typedef struct { tinynv_cmdbuf_t c; uint64_t va; uint64_t reserved; } batch_t;

static int batch_begin(tinynv_device_t d, batch_t *b, uint32_t dwords) {
  dwords += 16;   // the closing release
  uint8_t *h = ring_take(d, (uint64_t)dwords * 4, 256, &b->va);
  if (!h) return -1;
  b->c.words = (uint32_t *)h;
  b->c.n = 0;
  b->c.cap = dwords;
  b->reserved = (uint64_t)dwords * 4;
  return 0;
}

static int batch_submit(tinynv_device_t d, batch_t *b) {
  uint64_t value = d->seq + 1;
  if (tinynv_cmd_release(&b->c, d->sem.va, value)) return -1;
  // give back what the batch did not use, when nothing was taken after it
  uint64_t used = ((uint64_t)b->c.n * 4 + 255) & ~255ull;
  if (d->ring.va + d->ring_off == b->va + b->reserved) d->ring_off -= b->reserved - used;

  AmpereAControlGPFifo *u = (AmpereAControlGPFifo *)d->userd.cpu;
  uint32_t next = (d->gp_put + 1) % GPFIFO_ENTRIES;
  double t0 = tinynv_now_s();
  while (next == __atomic_load_n(&u->GPGet, __ATOMIC_ACQUIRE)) {
    if (channel_error(d)) return -1;
    if (tinynv_now_s() - t0 > WAIT_TIMEOUT_S) { d->dead = 1; return tinynv_fail("the GPU stopped taking work (ring full)"); }
    sched_yield();
  }
  ((volatile uint64_t *)d->gpfifo.cpu)[d->gp_put] = tinynv_ring_entry(b->va, b->c.n);
  d->gp_put = next;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  u->GPPut = d->gp_put;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  d->usermode[NVC361_NOTIFY_CHANNEL_PENDING / 4] = d->token;
  d->seq = value;
  d->batches++;
  return 0;
}

// *** local memory, sized for the whole die, grown when a kernel wants more per thread

static int ensure_local_memory(tinynv_device_t d, uint32_t per_thread) {
  if (per_thread <= d->slm_per_thread) return 0;
  uint32_t want = (per_thread + 31) & ~31u;
  tinynv_die_t die = {.num_gpcs = d->num_gpcs, .num_tpc_per_gpc = d->tpc_per_gpc, .num_sm_per_tpc = d->sm_per_tpc,
                      .max_warps_per_sm = d->max_warps};
  uint64_t per_tpc = 0, total = tinynv_local_memory_size(&die, want, &per_tpc);
  mem_t slm;
  if (mem_alloc(d, total, 0, 0, &slm)) return -1;
  if (sync_all(d)) { mem_free(d, &slm); return -1; }
  if (d->slm.hphys) mem_free(d, &d->slm);
  d->slm = slm;
  d->slm_per_thread = want;
  batch_t b;
  if (batch_begin(d, &b, 16) || tinynv_cmd_local_memory(&b.c, d->slm.va, per_tpc)) return -1;
  TRACE("local memory %llu bytes (%u per thread, %llu per TPC)\n", (unsigned long long)total, want, (unsigned long long)per_tpc);
  return batch_submit(d, &b);
}

// *** launching: macuda's descriptor, written into the ring, run by two methods

static int build_qmd(tinynv_device_t d, tinynv_module_t m, const tinynv_image_t *layout, const tinynv_kernel_desc_t *k,
                     const uint32_t grid[3], const uint32_t block[3], uint32_t dyn_smem, uint32_t cbuf0_bytes,
                     uint64_t cbuf0_va, tinynv_qmd_t *out) {
  tinynv_qmd_t q;
  memset(&q, 0, sizeof(q));
  q.v3 = d->cls_compute < BLACKWELL_COMPUTE_A;
  tinynv_qmd_program_t prog = {.regs = k->regs,
                               .shmem = (uint32_t)((0x400 + k->static_smem + dyn_smem + 127) & ~127u),
                               .slm_per_thread = d->slm_per_thread,
                               .prog_size = (uint32_t)k->text_size,
                               .sass_version = tinynv_sass_version(d->sm_version),
                               .barriers = k->num_barriers};
  if (prog.barriers > 31) return tinynv_fail("kernel %s declares %u barriers; the descriptor holds at most 31", k->name, prog.barriers);
  tinynv_qmd_launch_t l = {.program_addr = m->mem.va + layout->text_off};
  for (int i = 0; i < 3; i++) { l.grid[i] = grid[i]; l.block[i] = block[i]; }
  prog.constbuf_used[0] = 1;
  prog.membar = TINYNV_QMD_MEMBAR_SYS;
  prog.invalidate = TINYNV_QMD_INVALIDATE_ALL;
  prog.constbuf_size[0] = layout->constbuf[0].used ? (uint32_t)layout->constbuf[0].size : cbuf0_bytes;
  l.constbuf_addr[0] = cbuf0_va;
  l.constbuf_set[0] = 1;
  for (int i = 1; i < TINYNV_QMD_CONSTBUFS; i++) {
    if (!layout->constbuf[i].used) continue;
    prog.constbuf_used[i] = 1;
    prog.constbuf_size[i] = (uint32_t)layout->constbuf[i].size;
    l.constbuf_addr[i] = m->mem.va + layout->constbuf[i].off;
    l.constbuf_set[i] = 1;
  }
  if (tinynv_qmd_program(&q, &prog) || tinynv_qmd_launch(&q, &l)) return -1;
  if (q.overlaps) return tinynv_fail("%u bits of the descriptor were claimed twice", q.overlaps);
  *out = q;
  return 0;
}

static int run_kernel(tinynv_device_t d, tinynv_kernel_t k, const uint32_t grid[3], const uint32_t block[3],
                      uint32_t dyn_smem, const void *params, size_t params_len);
static int htod_locked(tinynv_device_t d, uint64_t dst, const void *src, size_t n);
static int memset_locked(tinynv_device_t d, uint64_t dst, int v, size_t n);

// The module's image goes to video memory once. The copy kernel's own module cannot be copied by itself, so it (and
// only it) travels inside the command stream as inline data.
static int upload_inline(tinynv_device_t d, uint64_t dst, const void *src, size_t n) {
  const uint8_t *s = src;
  for (size_t off = 0; off < n;) {
    uint32_t chunk = (uint32_t)((n - off) > 16384 ? 16384 : (n - off));
    uint32_t padded = (chunk + 3) & ~3u;
    uint8_t tmp[16384];
    memset(tmp, 0, sizeof(tmp));
    memcpy(tmp, s + off, chunk);
    batch_t b;
    if (batch_begin(d, &b, TINYNV_INLINE_DWORDS(padded) + 8) ||
        tinynv_cmd_inline_upload(&b.c, dst + off, tmp, padded) || batch_submit(d, &b)) return -1;
    off += chunk;
  }
  return 0;
}

static int copy_kernel(tinynv_device_t d, uint64_t dst, uint64_t src, uint64_t n) {
  if (!n) return 0;
  uint64_t units = n / 16 + 1, blocks = (units + 255) / 256;
  if (blocks > 4096) blocks = 4096;
  if (blocks < 1) blocks = 1;
  uint32_t grid[3] = {(uint32_t)blocks, 1, 1}, block[3] = {256, 1, 1};
  uint64_t p[3] = {dst, src, n};
  return run_kernel(d, d->copy_k, grid, block, 0, p, sizeof(p));
}

static int module_ensure_loaded(tinynv_device_t d, tinynv_module_t m) {
  if (m->loaded) return 0;
  if (!m->cubin.nkernels) return tinynv_fail("module has no kernels");
  if (tinynv_cubin_image(&m->cubin, m->cubin.kernels[0].name, 1, &m->layout)) return -1;
  uint64_t bytes = ((m->layout.len + 0xfff) & ~0xfffull) + 0x1000;   // guard: the prefetcher reads past the end
  int rc = mem_alloc(d, bytes, 0, 0, &m->mem);
  if (rc) { tinynv_image_free(&m->layout); return -1; }
  if (tinynv_image_relocate(&m->layout, m->mem.va)) goto fail;
  // zero the tail first, then the image over it
  if (m == d->copy_mod) {
    uint8_t *buf = calloc(1, bytes);
    memcpy(buf, m->layout.bytes, m->layout.len);
    rc = upload_inline(d, m->mem.va, buf, bytes);
    free(buf);
    if (rc) goto fail;
  } else {
    if (memset_locked(d, m->mem.va, 0, bytes)) goto fail;
    if (htod_locked(d, m->mem.va, m->layout.bytes, m->layout.len)) goto fail;
  }
  m->loaded = 1;
  TRACE("module image %zu bytes at %#llx\n", m->layout.len, (unsigned long long)m->mem.va);
  return 0;
fail:
  mem_free(d, &m->mem);
  tinynv_image_free(&m->layout);
  return -1;
}

static int run_kernel(tinynv_device_t d, tinynv_kernel_t k, const uint32_t grid[3], const uint32_t block[3],
                      uint32_t dyn_smem, const void *params, size_t params_len) {
  const tinynv_kernel_desc_t *kd = k->desc;
  if (module_ensure_loaded(d, k->mod)) return -1;
  if (!k->has_layout) {
    if (tinynv_cubin_image(&k->mod->cubin, kd->name, 0, &k->layout)) return -1;
    k->has_layout = 1;
  }
  if (ensure_local_memory(d, kd->min_stack + 0x240)) return -1;
  uint32_t cbuf0_bytes = (uint32_t)(kd->param_base + params_len);
  uint32_t mind = d->cls_compute < BLACKWELL_COMPUTE_A ? TINYNV_QMD_V3_CBUF0_MIN_DWORDS : TINYNV_QMD_CBUF0_MIN_DWORDS;
  if (cbuf0_bytes < mind * 4) cbuf0_bytes = mind * 4;
  cbuf0_bytes = (cbuf0_bytes + 0xff) & ~0xffu;
  const uint64_t slot = TINYNV_QMD_SLOT_BYTES;
  // The descriptor, its constant buffer and the batch that launches it are ONE piece of the ring. Taken separately, a
  // wrap between the two takes would let later work reach the descriptor's bytes before the GPU had read them.
  const uint32_t batch_dwords = 32;
  uint64_t qmd_va;
  uint8_t *host = ring_take(d, slot + cbuf0_bytes + (uint64_t)batch_dwords * 4, 256, &qmd_va);
  if (!host) return -1;
  tinynv_qmd_t q;
  if (build_qmd(d, k->mod, &k->layout, kd, grid, block, dyn_smem, cbuf0_bytes, qmd_va + slot, &q)) return -1;
  memcpy(host, q.b, TINYNV_QMD_BYTES);
  memset(host + TINYNV_QMD_BYTES, 0, slot - TINYNV_QMD_BYTES);
  memset(host + slot, 0, cbuf0_bytes);
  tinynv_qmd_cbuf0((uint32_t *)(host + slot), cbuf0_bytes / 4, q.v3, SHARED_WINDOW, LOCAL_WINDOW, grid, block);
  if (params_len) memcpy(host + slot + kd->param_base, params, params_len);
  batch_t b;
  b.c.words = (uint32_t *)(host + slot + cbuf0_bytes);   // 256-aligned: slot and cbuf0_bytes both are
  b.c.n = 0;
  b.c.cap = batch_dwords;
  b.va = qmd_va + slot + cbuf0_bytes;
  b.reserved = (uint64_t)batch_dwords * 4;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (tinynv_cmd_memory_barrier(&b.c) || tinynv_cmd_launch(&b.c, qmd_va)) return -1;
  d->launches++;
  return batch_submit(d, &b);
}

// *** bring-up

static int pick_class(const NvU32 *have, NvU32 n, const NvU32 *want, int nwant) {
  for (int w = 0; w < nwant; w++)
    for (NvU32 i = 0; i < n; i++) if (have[i] == want[w]) return (int)want[w];
  return 0;
}

static int bring_up(tinynv_device_t d) {
  NV_STATUS st;
  if (nvrm_open(&d->ctl, 1)) return tinynv_fail("no NVIDIA RM service (is the NullMoth driver installed and the card up?)");
  if (nvrm_open(&d->devc, 0)) return tinynv_fail("could not open the device connection");

  nv_ioctl_card_info_t ci[1];
  memset(ci, 0, sizeof(ci));
  if (nvrm_esc(&d->ctl, NV_ESC_CARD_INFO, ci, sizeof(ci)) || !ci[0].valid)
    return tinynv_fail("the driver reports no NVIDIA card (is the adapter up? NullMoth's own tools say 'nvrmctl go 2')");
  TRACE("card %04x:%04x gpu_id 0x%x via %s\n", ci[0].pci_info.vendor_id, ci[0].pci_info.device_id, ci[0].gpu_id, nvrm_service_name());

  NVOS21_PARAMETERS root = {.hRoot = 0, .hObjectParent = 0, .hObjectNew = 0, .hClass = NV01_ROOT_CLIENT, .pAllocParms = NULL};
  if (nvrm_esc(&d->ctl, NV_ESC_RM_ALLOC, &root, sizeof(root)) || root.status) return rm_fail("creating an RM client", root.status);
  d->hClient = root.hObjectNew;

  NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS idp = {.gpuId = ci[0].gpu_id};
  if ((st = rm_control(d, d->hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &idp, sizeof(idp)))) return rm_fail("GPU id info", st);
  NV0080_ALLOC_PARAMETERS ap0080 = {.deviceId = idp.deviceInstance, .hClientShare = d->hClient};
  NV2080_ALLOC_PARAMETERS ap2080 = {.subDeviceId = idp.subDeviceInstance};
  if ((st = rm_alloc(d, d->hClient, &d->hDevice, NV01_DEVICE_0, &ap0080))) return rm_fail("device object", st);
  if ((st = rm_alloc(d, d->hDevice, &d->hSubdevice, NV20_SUBDEVICE_0, &ap2080))) return rm_fail("subdevice object", st);

  NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS np = {.gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII};
  if (rm_control(d, d->hSubdevice, NV2080_CTRL_CMD_GPU_GET_NAME_STRING, &np, sizeof(np)) == NV_OK)
    snprintf(d->name, sizeof(d->name), "%s", (const char *)np.gpuNameString.ascii);
  else snprintf(d->name, sizeof(d->name), "NVIDIA %04x", ci[0].pci_info.device_id);

  static NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS cl;
  memset(&cl, 0, sizeof(cl));
  if ((st = rm_control(d, d->hDevice, NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &cl, sizeof(cl)))) return rm_fail("class list", st);
  static const NvU32 gpfifos[] = {BLACKWELL_CHANNEL_GPFIFO_B, BLACKWELL_CHANNEL_GPFIFO_A, 0xC86F /*HOPPER*/, AMPERE_CHANNEL_GPFIFO_A};
  static const NvU32 computes[] = {BLACKWELL_COMPUTE_B, BLACKWELL_COMPUTE_A, 0xC9C0 /*ADA*/, 0xC7C0 /*AMPERE_B*/, 0xC6C0 /*AMPERE_A*/};
  static const NvU32 usermodes[] = {TURING_USERMODE_A};
  d->cls_gpfifo = pick_class(cl.classList, cl.numClasses, gpfifos, 4);
  d->cls_compute = pick_class(cl.classList, cl.numClasses, computes, 5);
  d->cls_usermode = pick_class(cl.classList, cl.numClasses, usermodes, 1);
  if (!d->cls_gpfifo || !d->cls_compute || !d->cls_usermode)
    return tinynv_fail("this card offers no supported channel/compute/usermode class (gpfifo 0x%x compute 0x%x usermode 0x%x)",
                       d->cls_gpfifo, d->cls_compute, d->cls_usermode);
  TRACE("classes: gpfifo 0x%x compute 0x%x usermode 0x%x\n", d->cls_gpfifo, d->cls_compute, d->cls_usermode);

  static NV0080_CTRL_GR_GET_INFO_V2_PARAMS gr;
  memset(&gr, 0, sizeof(gr));
  const NvU32 idx[] = {NV0080_CTRL_GR_INFO_INDEX_SM_VERSION, NV0080_CTRL_GR_INFO_INDEX_LITTER_NUM_GPCS,
                       NV0080_CTRL_GR_INFO_INDEX_LITTER_NUM_TPC_PER_GPC, NV0080_CTRL_GR_INFO_INDEX_LITTER_NUM_SM_PER_TPC,
                       NV0080_CTRL_GR_INFO_INDEX_MAX_WARPS_PER_SM};
  gr.grInfoListSize = 5;
  for (int i = 0; i < 5; i++) gr.grInfoList[i].index = idx[i];
  if ((st = rm_control(d, d->hDevice, NV0080_CTRL_CMD_GR_GET_INFO_V2, &gr, sizeof(gr)))) return rm_fail("GR info", st);
  d->sm_version = gr.grInfoList[0].data; d->num_gpcs = gr.grInfoList[1].data; d->tpc_per_gpc = gr.grInfoList[2].data;
  d->sm_per_tpc = gr.grInfoList[3].data; d->max_warps = gr.grInfoList[4].data;
  // enabled SMs, not the die's maximum: the masks of GPCs and of TPCs within each (the 5090 has 170 of 192)
  {
    NV2080_CTRL_GR_GET_GPC_MASK_PARAMS gm;
    memset(&gm, 0, sizeof(gm));
    if (rm_control(d, d->hSubdevice, NV2080_CTRL_CMD_GR_GET_GPC_MASK, &gm, sizeof(gm)) == NV_OK) {
      uint32_t tpcs = 0;
      for (uint32_t g = 0; g < 32; g++) {
        if (!(gm.gpcMask & (1u << g))) continue;
        NV2080_CTRL_GR_GET_TPC_MASK_PARAMS tm;
        memset(&tm, 0, sizeof(tm));
        tm.gpcId = g;
        if (rm_control(d, d->hSubdevice, NV2080_CTRL_CMD_GR_GET_TPC_MASK, &tm, sizeof(tm)) == NV_OK)
          tpcs += (uint32_t)__builtin_popcount(tm.tpcMask);
      }
      d->sm_count = tpcs * d->sm_per_tpc;
    }
  }
  TRACE("sm_version 0x%x, %u GPCs x %u TPCs x %u SMs, %u warps/SM, %u SMs enabled\n", d->sm_version, d->num_gpcs,
        d->tpc_per_gpc, d->sm_per_tpc, d->max_warps, d->sm_count);

  static NV2080_CTRL_FB_GET_INFO_V2_PARAMS fb;
  memset(&fb, 0, sizeof(fb));
  fb.fbInfoListSize = 2;
  fb.fbInfoList[0].index = NV2080_CTRL_FB_INFO_INDEX_USABLE_RAM_SIZE;
  fb.fbInfoList[1].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE;
  if (rm_control(d, d->hSubdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fb, sizeof(fb)) == NV_OK) {
    d->vram_total = (uint64_t)fb.fbInfoList[0].data << 10;
    d->vram_free_at_open = (uint64_t)fb.fbInfoList[1].data << 10;
  }

  // doorbell page
  if ((st = rm_alloc(d, d->hSubdevice, &d->hUsermode, d->cls_usermode, NULL))) return rm_fail("usermode object", st);
  if (nvrm_open(&d->umc, 0)) return tinynv_fail("usermode connection");
  {
    nv_ioctl_nvos33_parameters_with_fd cp = {
      .params = {.hClient = d->hClient, .hDevice = d->hSubdevice, .hMemory = d->hUsermode, .offset = 0, .length = 4096, .flags = 0},
      .fd = d->umc.fd};
    if (nvrm_esc(&d->ctl, NV_ESC_RM_MAP_MEMORY, &cp, sizeof(cp)) || cp.params.status) return rm_fail("mapping the doorbell", cp.params.status);
    d->usermode = nvrm_map(&d->umc, NULL, 0);
    if (!d->usermode) return tinynv_fail("mapping the doorbell page");
  }

  NV_VASPACE_ALLOCATION_PARAMETERS vap = {.flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS, .vaBase = 0x200000000ull};
  if ((st = rm_alloc(d, d->hDevice, &d->hVaSpace, FERMI_VASPACE_A, &vap))) return rm_fail("GPU address space", st);

  if (mem_alloc(d, 0x1000, 1, 1, &d->notifier) || mem_alloc(d, 0x80000, 0, 1, &d->userd) ||
      mem_alloc(d, GPFIFO_ENTRIES * 8, 1, 1, &d->gpfifo) || mem_alloc(d, 0x1000, 1, 1, &d->sem) ||
      mem_alloc(d, RING_BYTES, 1, 1, &d->ring) || mem_alloc(d, STAGE_BYTES, 1, 1, &d->stage))
    return -1;
  memset(d->notifier.cpu, 0, 0x1000);
  memset(d->sem.cpu, 0, 0x1000);
  memset(d->userd.cpu, 0, 0x1000);

  NV_CONTEXT_DMA_ALLOCATION_PARAMS cdp = {
    .flags = DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL) | DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE),
    .hMemory = d->notifier.hphys, .offset = 0, .limit = d->notifier.size - 1};
  if ((st = rm_alloc(d, d->hDevice, &d->hCtxDma, NV01_CONTEXT_DMA, &cdp))) return rm_fail("error notifier", st);

  NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS tsg = {.hVASpace = d->hVaSpace, .engineType = NV2080_ENGINE_TYPE_GRAPHICS};
  NvHandle parent = d->hDevice;
  if (rm_alloc(d, d->hDevice, &d->hTsg, KEPLER_CHANNEL_GROUP_A, &tsg) == NV_OK) {
    NV_CTXSHARE_ALLOCATION_PARAMETERS cs = {.hVASpace = d->hVaSpace, .flags = NV_CTXSHARE_ALLOCATION_FLAGS_SUBCONTEXT_SYNC, .subctxId = 0};
    if (rm_alloc(d, d->hTsg, &d->hCtxShare, FERMI_CONTEXT_SHARE_A, &cs) == NV_OK) parent = d->hTsg;
    else { rm_free(d, d->hTsg); d->hTsg = 0; d->hCtxShare = 0; }
  }
  NV_CHANNEL_ALLOC_PARAMS chp;
  memset(&chp, 0, sizeof(chp));
  chp.hObjectError = d->hCtxDma;
  chp.gpFifoOffset = d->gpfifo.va;
  chp.gpFifoEntries = GPFIFO_ENTRIES;
  chp.flags = d->hCtxShare ? DRF_DEF(OS04, _FLAGS, _PRIVILEGED_CHANNEL, _TRUE) : 0;
  chp.hContextShare = d->hCtxShare;
  chp.hVASpace = d->hCtxShare ? 0 : d->hVaSpace;
  chp.hUserdMemory[0] = d->userd.hphys;
  chp.userdOffset[0] = 0;
  chp.engineType = NV2080_ENGINE_TYPE_GRAPHICS;
  st = rm_alloc(d, parent, &d->hChannel, d->cls_gpfifo, &chp);
  if (st && chp.flags) { chp.flags = 0; st = rm_alloc(d, parent, &d->hChannel, d->cls_gpfifo, &chp); }
  if (st) return rm_fail("compute channel", st);

  NVA06F_CTRL_BIND_PARAMS bp = {.engineType = NV2080_ENGINE_TYPE_GRAPHICS};
  if ((st = rm_control(d, d->hChannel, NVA06F_CTRL_CMD_BIND, &bp, sizeof(bp)))) return rm_fail("binding the channel", st);
  NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS sp = {.bEnable = NV_TRUE};
  if ((st = rm_control(d, d->hChannel, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &sp, sizeof(sp)))) return rm_fail("scheduling the channel", st);
  NVC36F_CTRL_GPFIFO_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX_PARAMS ni = {.index = NV_CHANNELGPFIFO_NOTIFICATION_TYPE_WORK_SUBMIT_TOKEN};
  rm_control(d, d->hChannel, NVC36F_CTRL_CMD_GPFIFO_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX, &ni, sizeof(ni));
  NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS tp = {0};
  if ((st = rm_control(d, d->hChannel, NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN, &tp, sizeof(tp)))) return rm_fail("work submit token", st);
  d->token = tp.workSubmitToken;

  if ((st = rm_alloc(d, d->hChannel, &d->hCompute, d->cls_compute, NULL))) return rm_fail("compute object", st);

  batch_t b;
  if (batch_begin(d, &b, 32) || tinynv_cmd_set_object(&b.c, 1, d->cls_compute) ||
      tinynv_cmd_shader_window(&b.c, 0, LOCAL_WINDOW) || tinynv_cmd_shader_window(&b.c, 1, SHARED_WINDOW) ||
      batch_submit(d, &b) || sync_all(d))
    return -1;
  TRACE("channel up: token 0x%x, first batch completed\n", d->token);

  // the copy kernel, which every transfer below uses
  if (tinynv_module_load(d, nvrmcu_copy1d_cubin, nvrmcu_copy1d_cubin_len, &d->copy_mod) != TINYNV_OK) return -1;
  if (tinynv_get_kernel(d->copy_mod, "tinycudart_copy1d", &d->copy_k) != TINYNV_OK) return -1;
  if (module_ensure_loaded(d, d->copy_mod) || sync_all(d)) return -1;
  d->up = 1;
  return 0;
}

// At exit: let queued work finish, then free the RM client, which frees every object under it (channel, memory,
// address space). The kext would reclaim them when the connection closes, but a client that leaves tidily cannot leave
// video memory behind if that path has a gap.
static void shutdown_at_exit(void) {
  tinynv_device_t d = &g_dev;
  if (!d->hClient) return;
  if (d->up && !d->dead) {
    double t0 = tinynv_now_s();
    while (sem_done(d) < d->seq && tinynv_now_s() - t0 < 2.0) sched_yield();
  }
  NVOS00_PARAMETERS p = {.hRoot = d->hClient, .hObjectParent = 0, .hObjectOld = d->hClient};
  nvrm_esc(&d->ctl, NV_ESC_RM_FREE, &p, sizeof(p));
  d->hClient = 0;
  d->up = 0;
}

// *** the interface

tinynv_status_t tinynv_init(void) {
  pthread_mutex_lock(&g_lock);
  if (!g_init_done) {
    g_init_done = 1;
    g_init_status = bring_up(&g_dev) ? TINYNV_ERR_NODEV : TINYNV_OK;
    if (g_dev.hClient) atexit(shutdown_at_exit);
    if (g_init_status != TINYNV_OK) fprintf(stderr, "nvrmcu: GPU not available: %s\n", tinynv_last_error());
    else if (getenv("NVRMCU_QUIET") == NULL)
      fprintf(stderr, "nvrmcu: %s (sm_%u%u, %u SMs, %llu MB) through %s\n", g_dev.name, (g_dev.sm_version >> 8) & 0xf,
              g_dev.sm_version == 0xa04 ? 0 : (g_dev.sm_version & 0xf), g_dev.sm_count,
              (unsigned long long)(g_dev.vram_total >> 20), nvrm_service_name());
  }
  pthread_mutex_unlock(&g_lock);
  return g_init_status;
}

int tinynv_device_count(void) { return tinynv_init() == TINYNV_OK ? 1 : 0; }

tinynv_status_t tinynv_device_get(tinynv_device_t *out, int ordinal) {
  if (ordinal != 0 || tinynv_init() != TINYNV_OK) return TINYNV_ERR_NODEV;
  *out = &g_dev;
  return TINYNV_OK;
}

tinynv_status_t tinynv_device_props(tinynv_device_t d, tinynv_device_props_t *p) {
  memset(p, 0, sizeof(*p));
  if (!d || !d->up) return TINYNV_ERR_NODEV;
  p->warp_size = 32;
  p->max_threads_per_block = 1024;
  p->max_shared_per_block = 48 * 1024;
  p->max_shared_per_sm = TINYNV_SMEM_MAX_PER_BLOCK;
  unsigned sm = d->sm_version, lo = sm & 0xff;
  if (sm == 0xa04) { p->cc_major = 12; p->cc_minor = 0; }
  else { p->cc_major = (int)((sm >> 8) & 0xff); p->cc_minor = (int)(lo > 0xf ? lo >> 4 : lo); }
  snprintf(p->name, sizeof(p->name), "%s", d->name);
  p->total_mem = d->vram_total;
  p->sm_count = d->sm_count ? (int)d->sm_count : (int)(d->num_gpcs * d->tpc_per_gpc * d->sm_per_tpc);
  return TINYNV_OK;
}

tinynv_status_t tinynv_module_load(tinynv_device_t d, const void *cubin, size_t len, tinynv_module_t *out) {
  if (!cubin || !len) return TINYNV_ERR_INVALID;
  tinynv_module_t m = calloc(1, sizeof(*m));
  m->dev = d;
  m->image = malloc(len);
  memcpy(m->image, cubin, m->len = len);
  if (tinynv_cubin_parse(m->image, m->len, &m->cubin)) { free(m->image); free(m); return TINYNV_ERR_INVALID; }
  *out = m;
  return TINYNV_OK;
}

tinynv_status_t tinynv_module_unload(tinynv_module_t m) {
  if (!m) return TINYNV_ERR_INVALID;
  pthread_mutex_lock(&g_lock);
  if (m->loaded) {
    if (sync_all(m->dev)) { pthread_mutex_unlock(&g_lock); return TINYNV_ERR_DRIVER; }
    mem_free(m->dev, &m->mem);
    tinynv_image_free(&m->layout);
  }
  pthread_mutex_unlock(&g_lock);
  tinynv_cubin_free(&m->cubin);
  free(m->image);
  free(m);
  return TINYNV_OK;
}

tinynv_status_t tinynv_get_global(tinynv_module_t m, const char *name, tinynv_devptr_t *addr, size_t *size) {
  if (!m || !name || !addr || !size) return TINYNV_ERR_INVALID;
  uint64_t off = 0, sz = 0;
  if (tinynv_cubin_global(&m->cubin, name, &off, &sz)) return TINYNV_ERR_INVALID;
  pthread_mutex_lock(&g_lock);
  int rc = module_ensure_loaded(m->dev, m);
  pthread_mutex_unlock(&g_lock);
  if (rc) return TINYNV_ERR_DRIVER;
  if (off + sz > m->layout.len) return tinynv_fail("global '%s' falls outside the image", name), TINYNV_ERR_DRIVER;
  *addr = m->mem.va + off;
  *size = (size_t)sz;
  return TINYNV_OK;
}

tinynv_status_t tinynv_get_kernel(tinynv_module_t m, const char *name, tinynv_kernel_t *out) {
  if (!m || !name) return TINYNV_ERR_INVALID;
  const tinynv_kernel_desc_t *kd = tinynv_cubin_kernel(&m->cubin, name);
  if (!kd) return TINYNV_ERR_INVALID;
  tinynv_kernel_t k = calloc(1, sizeof(*k));
  k->mod = m;
  k->desc = kd;
  *out = k;
  return TINYNV_OK;
}

tinynv_status_t tinynv_kernel_info(tinynv_kernel_t k, tinynv_kernel_info_t *i) {
  if (!k) return TINYNV_ERR_INVALID;
  memset(i, 0, sizeof(*i));
  const tinynv_kernel_desc_t *kd = k->desc;
  i->param_base = (int)kd->param_base;
  i->regs = (int)kd->regs;
  i->static_smem = (int)kd->static_smem;
  i->max_threads = (int)tinynv_kernel_thread_limit(kd);
  const tinynv_cubin_t *cb = &k->mod->cubin;
  i->const_size_bytes = (int)cb->const3_size;
  i->local_size_bytes = (int)kd->min_stack;
  i->cluster_size_must_be_set = kd->explicit_cluster ? 1 : 0;
  for (int c = 0; c < 3; c++) i->required_cluster[c] = (int)kd->cluster_dim[c];
  i->attr_reported = (1u << 2) | (1u << 3) | (1u << 7) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13);
  if (cb->ptx_version >= 0) { i->ptx_version = cb->ptx_version; i->attr_reported |= 1u << 5; }
  if (!i->max_threads) return tinynv_fail("kernel %s uses too many registers for any block", kd->name), TINYNV_ERR_INVALID;
  int n = kd->nparams;
  if (n > (int)(sizeof(i->params) / sizeof(i->params[0]))) return tinynv_fail("kernel %s has %d parameters", kd->name, n), TINYNV_ERR_INVALID;
  i->num_params = n;
  for (int j = 0; j < n; j++) { i->params[j].offset = (int)kd->params[j].offset; i->params[j].size = (int)kd->params[j].size; }
  return TINYNV_OK;
}

// *** memory

tinynv_status_t tinynv_malloc(tinynv_device_t d, size_t n, tinynv_devptr_t *out) {
  if (!d || !d->up) return TINYNV_ERR_NODEV;
  if (!n) n = 1;
  pthread_mutex_lock(&g_lock);
  if (d->nallocs == d->cap_allocs) {
    d->cap_allocs = d->cap_allocs ? d->cap_allocs * 2 : 256;
    d->allocs = realloc(d->allocs, sizeof(alloc_t) * (size_t)d->cap_allocs);
  }
  alloc_t *a = &d->allocs[d->nallocs];
  int rc = mem_alloc(d, n, 0, 0, &a->m);
  if (!rc) { a->va = a->m.va; d->nallocs++; d->live_bytes += a->m.size; *out = a->va; }
  pthread_mutex_unlock(&g_lock);
  return rc == 0 ? TINYNV_OK : rc == -2 ? TINYNV_ERR_OOM : TINYNV_ERR_DRIVER;
}

tinynv_status_t tinynv_free(tinynv_device_t d, tinynv_devptr_t p) {
  if (!d || !d->up) return TINYNV_ERR_NODEV;
  if (!p) return TINYNV_OK;
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < d->nallocs; i++) {
    if (d->allocs[i].va != p) continue;
    if (sync_all(d)) { pthread_mutex_unlock(&g_lock); return TINYNV_ERR_DRIVER; }
    d->live_bytes -= d->allocs[i].m.size;
    mem_free(d, &d->allocs[i].m);
    d->allocs[i] = d->allocs[--d->nallocs];
    pthread_mutex_unlock(&g_lock);
    return TINYNV_OK;
  }
  pthread_mutex_unlock(&g_lock);
  return tinynv_fail("free of %#llx, which this driver did not allocate", (unsigned long long)p), TINYNV_ERR_INVALID;
}

// staging space; wraps (and waits for everything) when full
static uint8_t *stage_take(tinynv_device_t d, uint64_t n, uint64_t *va) {
  if (d->stage_off + n > d->stage.size) {
    if (sync_all(d)) return NULL;
    d->stage_off = 0;
  }
  uint64_t off = d->stage_off;
  d->stage_off = (off + n + 255) & ~255ull;
  *va = d->stage.va + off;
  return d->stage.cpu + off;
}

#define CHUNK (16ull << 20)

// The unlocked halves, for callers already holding the lock (module loading).
static int htod_locked(tinynv_device_t d, uint64_t dst, const void *src, size_t n) {
  const uint8_t *p = src;
  for (size_t off = 0; off < n;) {
    uint64_t c = n - off > CHUNK ? CHUNK : n - off, va;
    uint8_t *h = stage_take(d, c, &va);
    if (!h) return -1;
    memcpy(h, p + off, (size_t)c);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (copy_kernel(d, dst + off, va, c)) return -1;
    off += c;
  }
  return 0;
}

static int memset_locked(tinynv_device_t d, uint64_t dst, int v, size_t n) {
  if (!n) return 0;
  uint64_t c = n > CHUNK ? CHUNK : n, va = 0;
  uint8_t *h = stage_take(d, c, &va);
  if (!h) return -1;
  memset(h, v, (size_t)c);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  for (size_t off = 0; off < n; off += c)
    if (copy_kernel(d, dst + off, va, n - off > c ? c : n - off)) return -1;
  return 0;
}

tinynv_status_t tinynv_memcpy_htod(tinynv_stream_t s, tinynv_devptr_t dst, const void *src, size_t n) {
  if (!g_dev.up) return TINYNV_ERR_NODEV;
  pthread_mutex_lock(&g_lock);
  int rc = htod_locked(&g_dev, dst, src, n);
  pthread_mutex_unlock(&g_lock);
  return rc ? TINYNV_ERR_DRIVER : TINYNV_OK;
}

tinynv_status_t tinynv_memcpy_dtoh(tinynv_stream_t s, void *dst, tinynv_devptr_t src, size_t n) {
  tinynv_device_t d = &g_dev;
  pthread_mutex_lock(&g_lock);
  int rc = 0;
  uint8_t *p = dst;
  for (size_t off = 0; off < n && !rc;) {
    uint64_t c = n - off > CHUNK ? CHUNK : n - off, va;
    uint8_t *h = stage_take(d, c, &va);
    if (!h) { rc = -1; break; }
    rc = copy_kernel(d, va, src + off, c) || sync_all(d);
    if (!rc) memcpy(p + off, h, (size_t)c);
    off += c;
  }
  pthread_mutex_unlock(&g_lock);
  return rc ? TINYNV_ERR_DRIVER : TINYNV_OK;
}

tinynv_status_t tinynv_memcpy_dtod(tinynv_stream_t s, tinynv_devptr_t dst, tinynv_devptr_t src, size_t n) {
  tinynv_device_t d = &g_dev;
  pthread_mutex_lock(&g_lock);
  int rc = copy_kernel(d, dst, src, n);
  pthread_mutex_unlock(&g_lock);
  return rc ? TINYNV_ERR_DRIVER : TINYNV_OK;
}

tinynv_status_t tinynv_memset(tinynv_stream_t s, tinynv_devptr_t dst, int v, size_t n) {
  if (!g_dev.up) return TINYNV_ERR_NODEV;
  pthread_mutex_lock(&g_lock);
  int rc = memset_locked(&g_dev, dst, v, n);
  pthread_mutex_unlock(&g_lock);
  return rc ? TINYNV_ERR_DRIVER : TINYNV_OK;
}

tinynv_status_t tinynv_host_alloc(tinynv_device_t d, size_t n, void **out) { return TINYNV_ERR_INVALID; }   // shim falls back to pageable
tinynv_status_t tinynv_host_free(tinynv_device_t d, void *p) { return TINYNV_ERR_INVALID; }

tinynv_status_t tinynv_mem_info(tinynv_device_t d, uint64_t *free_bytes, uint64_t *total_bytes) {
  if (!d || !d->up) return TINYNV_ERR_NODEV;
  static NV2080_CTRL_FB_GET_INFO_V2_PARAMS fb;
  pthread_mutex_lock(&g_lock);
  memset(&fb, 0, sizeof(fb));
  fb.fbInfoListSize = 1;
  fb.fbInfoList[0].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE;
  uint64_t fr = d->vram_free_at_open > d->live_bytes ? d->vram_free_at_open - d->live_bytes : 0;
  if (rm_control(d, d->hSubdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fb, sizeof(fb)) == NV_OK) fr = (uint64_t)fb.fbInfoList[0].data << 10;
  pthread_mutex_unlock(&g_lock);
  *free_bytes = fr;
  *total_bytes = d->vram_total;
  return TINYNV_OK;
}

// *** streams and events: one in-order channel serves every stream

tinynv_status_t tinynv_stream_create(tinynv_device_t d, tinynv_stream_t *out) {
  tinynv_stream_t s = calloc(1, sizeof(*s));
  s->dev = d;
  *out = s;
  return TINYNV_OK;
}
tinynv_status_t tinynv_stream_destroy(tinynv_stream_t s) { free(s); return TINYNV_OK; }
tinynv_status_t tinynv_stream_sync(tinynv_stream_t s) {
  pthread_mutex_lock(&g_lock);
  int rc = sync_all(&g_dev);
  pthread_mutex_unlock(&g_lock);
  return rc ? TINYNV_ERR_DRIVER : TINYNV_OK;
}
int tinynv_stream_needs_wait(tinynv_stream_t s) { return sem_done(&g_dev) < g_dev.seq; }
tinynv_status_t tinynv_stream_flush(tinynv_stream_t s) { return TINYNV_OK; }
tinynv_status_t tinynv_stream_wait_event(tinynv_stream_t s, tinynv_event_t e) { return TINYNV_OK; }

tinynv_status_t tinynv_event_create(tinynv_device_t d, tinynv_event_t *out) {
  tinynv_event_t e = calloc(1, sizeof(*e));
  e->dev = d;
  *out = e;
  return TINYNV_OK;
}
tinynv_status_t tinynv_event_record(tinynv_event_t e, tinynv_stream_t s) {
  pthread_mutex_lock(&g_lock);
  e->value = g_dev.seq;
  pthread_mutex_unlock(&g_lock);
  return TINYNV_OK;
}
tinynv_status_t tinynv_event_sync(tinynv_event_t e) {
  pthread_mutex_lock(&g_lock);
  int rc = wait_seq(&g_dev, e->value);
  pthread_mutex_unlock(&g_lock);
  return rc ? TINYNV_ERR_DRIVER : TINYNV_OK;
}
tinynv_status_t tinynv_event_query(tinynv_event_t e, int *complete) {
  *complete = sem_done(&g_dev) >= e->value;
  return TINYNV_OK;
}
tinynv_status_t tinynv_event_destroy(tinynv_event_t e) { free(e); return TINYNV_OK; }
tinynv_status_t tinynv_event_elapsed(float *ms, tinynv_event_t a, tinynv_event_t b) {
  return tinynv_fail("event timing is not implemented in nvrmcu 0.1"), TINYNV_ERR_UNSUPPORTED;
}

// *** launch

tinynv_status_t tinynv_launch(tinynv_stream_t s, tinynv_kernel_t k, unsigned gx, unsigned gy, unsigned gz,
                              unsigned bx, unsigned by, unsigned bz, unsigned dyn_smem, const void *params, size_t params_len) {
  if (!k) return TINYNV_ERR_INVALID;
  const tinynv_kernel_desc_t *kd = k->desc;
  if (params_len > kd->param_size)
    return tinynv_fail("launch %s: %zu bytes of parameters, the cubin declares %u", kd->name, params_len, kd->param_size), TINYNV_ERR_INVALID;
  unsigned threads = bx * by * bz, bound = tinynv_kernel_thread_limit(kd);
  if (threads > bound) return tinynv_fail("launch refused: %u threads per block, the kernel allows %u - %s", threads, bound, kd->name), TINYNV_ERR_LAUNCH;
  if (0x400 + kd->static_smem + dyn_smem > TINYNV_SMEM_MAX_PER_BLOCK + 0x400)
    return tinynv_fail("launch refused: %u bytes of shared memory, at most %u - %s", kd->static_smem + dyn_smem,
                       TINYNV_SMEM_MAX_PER_BLOCK, kd->name), TINYNV_ERR_LAUNCH;
  uint32_t grid[3] = {gx, gy, gz}, block[3] = {bx, by, bz};
  pthread_mutex_lock(&g_lock);
  int rc = run_kernel(&g_dev, k, grid, block, dyn_smem, params, params_len);
  if (!rc && getenv("NVRMCU_SYNC")) rc = sync_all(&g_dev);
  pthread_mutex_unlock(&g_lock);
  return rc ? TINYNV_ERR_LAUNCH : TINYNV_OK;
}

// *** the rest of the interface: accepted, or refused by name

tinynv_status_t tinynv_set_download_kernel(tinynv_device_t d, tinynv_kernel_t k, unsigned block_threads, unsigned bpt) { return TINYNV_OK; }
tinynv_status_t tinynv_set_keepalive_kernel(tinynv_device_t d, tinynv_kernel_t k) { return TINYNV_OK; }
tinynv_status_t tinynv_graph_begin(tinynv_stream_t s) { return TINYNV_ERR_UNSUPPORTED; }
tinynv_status_t tinynv_graph_end(tinynv_stream_t s, tinynv_graph_t *out) { return TINYNV_ERR_UNSUPPORTED; }
tinynv_status_t tinynv_graph_launch(tinynv_stream_t s, tinynv_graph_t g) { return TINYNV_ERR_UNSUPPORTED; }
void tinynv_graph_free(tinynv_stream_t s, tinynv_graph_t g) {}
tinynv_status_t tinynv_sensors(tinynv_device_t d, tinynv_sensors_t *out) { memset(out, 0, sizeof(*out)); return TINYNV_ERR_UNSUPPORTED; }
int tinynv_device_gsp_free_heap(tinynv_device_t d, uint64_t *out) { return -1; }
int tinynv_device_wpr2_range(tinynv_device_t d, uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *e) { return -1; }
int tinynv_device_probe_phys_attr(tinynv_device_t d, uint64_t size, uint64_t offset, uint64_t *paddr, uint64_t *contig, unsigned *ap) { return -1; }
int tinynv_device_last_fault(tinynv_device_t d, uint64_t *va, uint32_t *type, uint32_t *access, uint32_t *sm_esr) { return 0; }
int tinynv_throttle_str(unsigned mask, const char **out, int max) { return 0; }

const char *tinynv_status_str(tinynv_status_t s) {
  switch (s) {
    case TINYNV_OK: return "ok";
    case TINYNV_ERR_NODEV: return "no such device";
    case TINYNV_ERR_OOM: return "out of memory";
    case TINYNV_ERR_LAUNCH: return "bad launch";
    case TINYNV_ERR_INVALID: return "invalid argument";
    case TINYNV_ERR_DRIVER: return "the driver could not do it: tinynv_last_error() says what went wrong";
    case TINYNV_ERR_UNSUPPORTED: return "not supported here";
  }
  return "unknown";
}

// Counters for the self-test's report.
void nvrmcu_stats(uint64_t *launches, uint64_t *batches) { *launches = g_dev.launches; *batches = g_dev.batches; }

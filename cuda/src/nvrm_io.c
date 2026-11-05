// nvrm_io.c - see nvrm_io.h.
#include "nvrm_io.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { SEL_ESC = 1, SEL_BINDFD = 3 };
static const char *g_service = "none";

int nvrm_trace(void) {
  static int t = -1;
  if (t < 0) { const char *e = getenv("NVRMCU_TRACE"); t = e && *e && *e != '0'; }
  return t;
}

const char *nvrm_service_name(void) { return g_service; }

static kern_return_t open_service(const char *name, uint32_t type, io_connect_t *out) {
  io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(name));
  if (!svc) return kIOReturnNotFound;
  kern_return_t kr = IOServiceOpen(svc, mach_task_self(), type, out);
  IOObjectRelease(svc);
  return kr;
}

int nvrm_open(nvrm_conn_t *c, int ctl) {
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  uint32_t t = ctl ? 0 : 1;
  io_connect_t conn = 0;
  // NVAccel is the path NullMoth's own Vulkan driver takes on Tahoe; NVRM direct is its fallback, so it is ours too.
  kern_return_t kr = open_service("NVAccel", 0x4E560000u | t, &conn);
  if (kr == KERN_SUCCESS) g_service = "NVAccel";
  else {
    kr = open_service("NVRM", t, &conn);
    if (kr == KERN_SUCCESS) g_service = "NVRM";
  }
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "nvrmcu: cannot open the NVIDIA kernel driver (NVAccel/NVRM): 0x%08x. Is the NullMoth driver loaded?\n", kr);
    errno = ENODEV;
    return -1;
  }
  // The kext keys mappings and RM file state by a descriptor number, as Linux does by the device file. Any descriptor
  // this process owns will do; a read-only /dev/null is what the kext's own user space uses.
  int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (fd < 0) { IOServiceClose(conn); return -1; }
  uint64_t in = (uint64_t)fd;
  kr = IOConnectCallScalarMethod(conn, SEL_BINDFD, &in, 1, NULL, NULL);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "nvrmcu: binding fd %d to the driver connection failed: 0x%08x\n", fd, kr);
    close(fd);
    IOServiceClose(conn);
    return -1;
  }
  c->conn = conn;
  c->fd = fd;
  c->ctl = ctl;
  return 0;
}

void nvrm_close(nvrm_conn_t *c) {
  if (c->conn) {
    uint64_t unbound = (uint64_t)(int64_t)-2;
    IOConnectCallScalarMethod(c->conn, SEL_BINDFD, &unbound, 1, NULL, NULL);
    IOServiceClose(c->conn);
  }
  if (c->fd >= 0) close(c->fd);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

int nvrm_esc(nvrm_conn_t *c, uint32_t cmd, void *params, uint32_t size) {
  uint64_t in = cmd;
  size_t out = size;
  kern_return_t kr = IOConnectCallMethod(c->conn, SEL_ESC, &in, 1, params, size, NULL, NULL, params, &out);
  if (nvrm_trace()) fprintf(stderr, "nvrmcu: esc 0x%02x %u bytes on %s -> 0x%08x\n", cmd, size, c->ctl ? "ctl" : "dev", kr);
  if (kr != KERN_SUCCESS) return -1;
  return 0;
}

void *nvrm_map(nvrm_conn_t *c, size_t *len, int write_combine) {
  mach_vm_address_t addr = 0;
  mach_vm_size_t sz = 0;
  kern_return_t kr = IOConnectMapMemory64(c->conn, 0, mach_task_self(), &addr, &sz,
                                          kIOMapAnywhere | (write_combine ? kIOMapWriteCombineCache : 0));
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "nvrmcu: mapping GPU memory into this process failed: 0x%08x\n", kr);
    return NULL;
  }
  if (len) *len = (size_t)sz;
  return (void *)(uintptr_t)addr;
}

void nvrm_unmap(nvrm_conn_t *c, void *addr) {
  if (c->conn && addr) IOConnectUnmapMemory64(c->conn, 0, mach_task_self(), (mach_vm_address_t)(uintptr_t)addr);
}

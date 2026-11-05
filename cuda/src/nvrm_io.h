// nvrm_io.h - user space to NullMoth's NVRM kext (NVIDIA's open RM ported to macOS), over IOKit.
//
// On Linux the RM is reached through /dev/nvidiactl and /dev/nvidia0 with ioctl() and mmap(). On macOS the kext
// publishes an IOService instead. These are the interface facts this file relies on, read from the kext's source
// (kexts/NVRM/NVRM.cpp) and its Vulkan user-space port:
//   - service "NVAccel" (open type 0x4E560000|t) or, failing that, "NVRM" (open type t); t = 0 ctl, 1 device
//   - selector 1 = escape: one scalar (the NV_ESC_* number), the parameter struct in and out
//   - selector 3 = bind a file-descriptor number to the connection (a /dev/null fd stands in for the device fd)
//   - selector 6 = wait for an RM event on the connection
//   - memory type 0 of a connection = the mapping prepared by the last NV_ESC_RM_MAP_MEMORY naming its fd
#ifndef NVRM_IO_H
#define NVRM_IO_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
  unsigned int conn;   // io_connect_t
  int fd;              // the /dev/null descriptor bound to it; RM structs that carry an fd carry this number
  int ctl;
} nvrm_conn_t;

int nvrm_open(nvrm_conn_t *c, int ctl);
void nvrm_close(nvrm_conn_t *c);
// Returns 0 when the kext accepted the escape. The RM's own verdict is the status field inside the struct.
int nvrm_esc(nvrm_conn_t *c, uint32_t cmd, void *params, uint32_t size);
// Map what the last NV_ESC_RM_MAP_MEMORY prepared on this connection. NULL on failure.
void *nvrm_map(nvrm_conn_t *c, size_t *len, int write_combine);
void nvrm_unmap(nvrm_conn_t *c, void *addr);
// How the kext was reached, for the report.
const char *nvrm_service_name(void);
int nvrm_trace(void);

#endif

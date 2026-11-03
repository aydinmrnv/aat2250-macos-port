/*
 * cuda-probe: loads NVIDIA's Linux libcuda through cuda-shim and runs the no-GPU gates.
 *
 *   cuda-probe /path/to/libcuda.so.615.78.08
 *
 * Gates (see README.md):
 *   L1  libcuda loads, relocates and its DT_INIT constructor returns
 *   L2  cuGetErrorString(CUDA_ERROR_INVALID_VALUE) gives NVIDIA's message; cuDriverGetVersion returns a version
 *   L3  cuInit(0) returns cleanly with an error (no NVIDIA device / driver here) instead of crashing or hanging
 */
#include "shim.h"

#include <stdio.h>
#include <stdlib.h>

typedef int CUresult;
typedef CUresult (*cuGetErrorString_t)(CUresult, const char **);
typedef CUresult (*cuGetErrorName_t)(CUresult, const char **);
typedef CUresult (*cuDriverGetVersion_t)(int *);
typedef CUresult (*cuInit_t)(unsigned);
typedef CUresult (*cuDeviceGetCount_t)(int *);

static const char *errname(cuGetErrorName_t f, CUresult r)
{
    const char *s = NULL;
    return f && f(r, &s) == 0 && s ? s : "?";
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s /path/to/libcuda.so.615.78.08\n", argv[0]); return 2; }
    shim_init();

    char err[512];
    struct elfobj *cuda = shim_load_library(argv[1], err, sizeof err);
    if (!cuda) { printf("L1 FAIL: %s\n", err); return 1; }
    shim_register_alias("libcuda.so.1", cuda);
    shim_register_alias("libcuda.so", cuda);
    printf("L1 PASS: loaded at 0x%lx, %zu MB, constructor returned\n", (unsigned long)elf_base(cuda), elf_span(cuda) >> 20);

    cuGetErrorString_t getstr = (cuGetErrorString_t)elf_sym(cuda, "cuGetErrorString");
    cuGetErrorName_t getname = (cuGetErrorName_t)elf_sym(cuda, "cuGetErrorName");
    cuDriverGetVersion_t getver = (cuDriverGetVersion_t)elf_sym(cuda, "cuDriverGetVersion");
    cuInit_t init = (cuInit_t)elf_sym(cuda, "cuInit");
    cuDeviceGetCount_t count = (cuDeviceGetCount_t)elf_sym(cuda, "cuDeviceGetCount");
    if (!getstr || !getver || !init || !count) { printf("L2 FAIL: missing exports\n"); return 1; }

    const char *msg = NULL;
    CUresult r = getstr(1 /* CUDA_ERROR_INVALID_VALUE */, &msg);
    int v = 0;
    CUresult rv = getver(&v);
    printf("L2 %s: cuGetErrorString(1) -> %d \"%s\"; cuDriverGetVersion -> %d, version %d\n",
           (r == 0 && msg && rv == 0 && v > 0) ? "PASS" : "FAIL", r, msg ? msg : "(null)", rv, v);

    fflush(stdout);
    CUresult ri = init(0);
    int n = -1;
    CUresult rc = count(&n);
    printf("L3 %s: cuInit(0) -> %d %s; cuDeviceGetCount -> %d %s, count %d\n", ri != 0 ? "PASS (clean error)" : "UNEXPECTED SUCCESS",
           ri, errname(getname, ri), rc, errname(getname, rc), n);
    return 0;
}

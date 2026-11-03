/*
 * cuda-shim glibc shim: the Linux C library as seen by an unmodified x86_64 Linux shared object, implemented on macOS.
 */
#ifndef CUDASHIM_SHIM_H
#define CUDASHIM_SHIM_H

#include "loader.h"

void shim_init(void);                           /* call once before loading anything */
void *shim_resolve(const char *name, int weak); /* elf_resolver for glibc imports */

/* dlopen registry used by the shim's dlopen/dlsym and by the host program */
struct elfobj *shim_load_library(const char *path, char *err, size_t errlen);
void shim_register_alias(const char *soname, struct elfobj *o);  /* e.g. "libcuda.so.1" -> the loaded libcuda */

int shim_trace(void);                           /* CUDASHIM_TRACE=1 */

#endif

/*
 * cuda-shim loader: maps an x86_64 Linux ELF shared object into a macOS x86_64 process.
 *
 * The object is loaded unmodified: segments are copied into memory as-is and only the dynamic relocations any ELF
 * loader applies are written. No NVIDIA binary is patched, disassembled or inspected beyond its ELF metadata.
 */
#ifndef CUDASHIM_LOADER_H
#define CUDASHIM_LOADER_H

#include <stddef.h>
#include <stdint.h>

struct elfobj;

/* Resolves one undefined symbol. Returns NULL if unknown; weak is 1 for STB_WEAK references. */
typedef void *(*elf_resolver)(const char *name, int weak);

struct elfobj *elf_load(const char *path, elf_resolver resolve, char *err, size_t errlen);
void *elf_sym(const struct elfobj *o, const char *name);          /* defined, exported symbol or NULL */
void elf_run_init(const struct elfobj *o, int argc, char **argv, char **envp);
void elf_run_fini(const struct elfobj *o);
const char *elf_path(const struct elfobj *o);
uintptr_t elf_base(const struct elfobj *o);
size_t elf_span(const struct elfobj *o);
/* PT_LOAD segment i (flags: PF_X 1, PF_W 2, PF_R 4); returns 0 past the last one */
int elf_segment(const struct elfobj *o, int i, uint64_t *vaddr, uint64_t *memsz, uint64_t *offset, uint32_t *flags);

/* Returns a trampoline that calls log(name) and then continues into target with all argument registers intact. */
void *elf_make_trace_stub(const char *name, void *target, void (*log)(const char *));

/* Called by an unresolved-import stub. Prints the import name and aborts. */
void elf_unresolved_trap(const char *name) __attribute__((noreturn));

#endif

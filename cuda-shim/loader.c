/*
 * cuda-shim loader (see loader.h).
 *
 * Supports what NVIDIA's 615.78.08 libcuda / libnvidia-ptxjitcompiler need, per their ELF metadata (C0 report):
 * ET_DYN x86_64, PT_LOAD segments, DT_HASH symbol lookup, RELA relocations of type RELATIVE, GLOB_DAT, JUMP_SLOT
 * and 64, DT_INIT / DT_INIT_ARRAY, no PT_TLS. Anything outside that is refused with an error, never guessed.
 */
#include "loader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ELF64 definitions (macOS ships no <elf.h>). Values from the System V ABI / x86_64 psABI. */
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version; uint64_t e_entry, e_phoff,
                 e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
                 e_shstrndx; } Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } Phdr;
typedef struct { int64_t d_tag; uint64_t d_val; } Dyn;
typedef struct { uint32_t st_name; unsigned char st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Rela;

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, ET_DYN = 3, EM_X86_64 = 62 };
enum { DT_NULL = 0, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_RELAENT = 9,
       DT_INIT = 12, DT_FINI = 13, DT_PLTRELSZ = 2, DT_JMPREL = 23, DT_PLTREL = 20, DT_TEXTREL = 22,
       DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28 };
enum { R_X86_64_NONE = 0, R_X86_64_64 = 1, R_X86_64_GLOB_DAT = 6, R_X86_64_JUMP_SLOT = 7, R_X86_64_RELATIVE = 8 };
enum { STB_WEAK = 2, SHN_UNDEF = 0 };

struct elfobj {
    char *path;
    uint8_t *base;
    size_t span;
    const Sym *symtab;
    const char *strtab;
    const uint32_t *hash;        /* DT_HASH: nbucket, nchain, buckets[], chains[] */
    uint64_t init, fini, init_array, init_arraysz, fini_array, fini_arraysz;
    int nseg;
    struct { uint64_t vaddr, memsz, offset; uint32_t flags; } seg[8];
};

/* --- unresolved-import stubs ------------------------------------------------------------------------------------ */

/* Each stub is: movabs rdi, <name>; movabs rax, elf_unresolved_trap; jmp rax (22 bytes, 24 with padding). */
enum { kStubSize = 24, kStubPage = 64 * 1024 };
static uint8_t *gStubs;
static size_t gStubUsed;

void elf_unresolved_trap(const char *name)
{
    fprintf(stderr, "cuda-shim: UNIMPLEMENTED import called: %s\n", name);
    abort();
}

static void *make_stub(const char *name)
{
    if (!gStubs || gStubUsed + kStubSize > kStubPage) {
        gStubs = mmap(NULL, kStubPage, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (gStubs == MAP_FAILED) { gStubs = NULL; return NULL; }
        gStubUsed = 0;
    }
    char *copy = strdup(name);
    if (!copy) return NULL;
    uint8_t *p = gStubs + gStubUsed;
    uint64_t a = (uint64_t)(uintptr_t)copy, t = (uint64_t)(uintptr_t)&elf_unresolved_trap;
    p[0] = 0x48; p[1] = 0xBF; memcpy(p + 2, &a, 8);       /* movabs rdi, imm64 */
    p[10] = 0x48; p[11] = 0xB8; memcpy(p + 12, &t, 8);    /* movabs rax, imm64 */
    p[20] = 0xFF; p[21] = 0xE0;                            /* jmp rax */
    p[22] = 0xCC; p[23] = 0xCC;
    gStubUsed += kStubSize;
    return p;
}

/* Call-tracing trampoline: saves the argument registers (rdi..r9, rax for varargs, xmm0-7), calls log(name),
 * restores them and jumps to target. At entry rsp % 16 == 8; seven pushes make it 0 for the call. */
enum { kTraceStubSize = 192 };   /* 161 bytes used */
void *elf_make_trace_stub(const char *name, void *target, void (*log)(const char *))
{
    static uint8_t *page;
    static size_t used;
    if (!page || used + kTraceStubSize > kStubPage) {
        page = mmap(NULL, kStubPage, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (page == MAP_FAILED) { page = NULL; return target; }
        used = 0;
    }
    uint8_t *p = page + used, *q = p;
    static const uint8_t save[] = {
        0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x50,         /* push rdi rsi rdx rcx r8 r9 rax */
        0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                     /* sub rsp, 128 */
        0xF3, 0x0F, 0x7F, 0x04, 0x24,                                 /* movdqu [rsp], xmm0 */
        0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x10, 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x30,
        0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x50, 0xF3, 0x0F, 0x7F, 0x74, 0x24, 0x60,
        0xF3, 0x0F, 0x7F, 0x7C, 0x24, 0x70,                           /* movdqu [rsp+16..112], xmm1..xmm7 */
    };
    static const uint8_t restore[] = {
        0xF3, 0x0F, 0x6F, 0x04, 0x24,                                 /* movdqu xmm0, [rsp] */
        0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x10, 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x30,
        0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x50, 0xF3, 0x0F, 0x6F, 0x74, 0x24, 0x60,
        0xF3, 0x0F, 0x6F, 0x7C, 0x24, 0x70,
        0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00,                     /* add rsp, 128 */
        0x58, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5A, 0x5E, 0x5F,         /* pop rax r9 r8 rcx rdx rsi rdi */
    };
    char *copy = strdup(name);
    if (!copy) return target;
    const uint64_t n = (uint64_t)(uintptr_t)copy, l = (uint64_t)(uintptr_t)log, t = (uint64_t)(uintptr_t)target;
    memcpy(q, save, sizeof save); q += sizeof save;
    *q++ = 0x48; *q++ = 0xBF; memcpy(q, &n, 8); q += 8;                /* movabs rdi, name */
    *q++ = 0x48; *q++ = 0xB8; memcpy(q, &l, 8); q += 8;                /* movabs rax, log */
    *q++ = 0xFF; *q++ = 0xD0;                                          /* call rax */
    memcpy(q, restore, sizeof restore); q += sizeof restore;
    *q++ = 0x49; *q++ = 0xBB; memcpy(q, &t, 8); q += 8;                /* movabs r11, target */
    *q++ = 0x41; *q++ = 0xFF; *q++ = 0xE3;                             /* jmp r11 */
    if ((size_t)(q - p) > kTraceStubSize) abort();
    used += kTraceStubSize;
    return p;
}

/* --- loading ---------------------------------------------------------------------------------------------------- */

static int fail(char *err, size_t n, const char *fmt, const char *a)
{
    snprintf(err, n, fmt, a);
    return 0;
}

static uint32_t sysv_hash(const char *s)
{
    uint32_t h = 0, g;
    while (*s) {
        h = (h << 4) + (unsigned char)*s++;
        if ((g = h & 0xF0000000u)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

void *elf_sym(const struct elfobj *o, const char *name)
{
    if (!o || !o->hash) return NULL;
    const uint32_t nbucket = o->hash[0];
    const uint32_t *bucket = o->hash + 2, *chain = bucket + nbucket;
    for (uint32_t i = bucket[sysv_hash(name) % nbucket]; i; i = chain[i]) {
        const Sym *s = &o->symtab[i];
        if (s->st_shndx != SHN_UNDEF && s->st_value && strcmp(o->strtab + s->st_name, name) == 0)
            return o->base + s->st_value;
    }
    return NULL;
}

static int relocate(struct elfobj *o, const Rela *r, size_t n, elf_resolver resolve, char *err, size_t errlen)
{
    for (size_t i = 0; i < n; i++) {
        const uint32_t type = (uint32_t)r[i].r_info, symi = (uint32_t)(r[i].r_info >> 32);
        if (r[i].r_offset + 8 > o->span) return fail(err, errlen, "relocation outside the image in %s", o->path);
        uint64_t *where = (uint64_t *)(o->base + r[i].r_offset);
        if (type == R_X86_64_NONE) continue;
        if (type == R_X86_64_RELATIVE) { *where = (uint64_t)(uintptr_t)o->base + (uint64_t)r[i].r_addend; continue; }
        if (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT) {
            char t[16];
            snprintf(t, sizeof t, "%u", type);
            return fail(err, errlen, "unsupported relocation type %s", t);
        }
        const Sym *s = &o->symtab[symi];
        const char *name = o->strtab + s->st_name;
        void *addr;
        if (s->st_shndx != SHN_UNDEF) {
            addr = o->base + s->st_value;                    /* the object's own definition */
        } else {
            const int weak = (s->st_info >> 4) == STB_WEAK;
            addr = resolve(name, weak);
            if (!addr && !weak) addr = make_stub(name);
            if (!addr && !weak) return fail(err, errlen, "could not create a stub for %s", name);
        }
        *where = (uint64_t)(uintptr_t)addr + (type == R_X86_64_64 ? (uint64_t)r[i].r_addend : 0);
    }
    return 1;
}

struct elfobj *elf_load(const char *path, elf_resolver resolve, char *err, size_t errlen)
{
    struct elfobj *o = NULL;
    uint8_t *img = MAP_FAILED;
    Phdr *ph = NULL;
    size_t span = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { snprintf(err, errlen, "open %s: %s", path, strerror(errno)); return NULL; }

    Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh || memcmp(eh.e_ident, "\177ELF", 4) != 0 ||
        eh.e_ident[4] != 2 || eh.e_ident[5] != 1 || eh.e_type != ET_DYN || eh.e_machine != EM_X86_64 ||
        eh.e_phentsize != sizeof(Phdr)) {
        fail(err, errlen, "%s is not an x86_64 ELF64 shared object", path);
        goto out;
    }
    ph = calloc(eh.e_phnum, sizeof *ph);
    if (!ph || pread(fd, ph, eh.e_phnum * sizeof *ph, (off_t)eh.e_phoff) != (ssize_t)(eh.e_phnum * sizeof *ph)) {
        fail(err, errlen, "cannot read program headers of %s", path);
        goto out;
    }
    const Phdr *dynph = NULL;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_TLS) { fail(err, errlen, "%s uses ELF TLS, which this loader does not support", path); goto out; }
        if (ph[i].p_type == PT_DYNAMIC) dynph = &ph[i];
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > span) span = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (!dynph || !span) { fail(err, errlen, "%s has no PT_DYNAMIC or no PT_LOAD", path); goto out; }
    span = (span + 0xFFF) & ~(size_t)0xFFF;

    /* One RWX anonymous region for the whole image; segments are copied in (no file mapping, so the 16 KB host page
     * size under Rosetta never meets 4 KB-aligned ELF segments). Tightening per-segment protections is a TODO. */
    img = mmap(NULL, span, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (img == MAP_FAILED) { fail(err, errlen, "cannot reserve memory for %s", path); goto out; }
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_filesz) continue;
        if (ph[i].p_filesz > ph[i].p_memsz ||
            pread(fd, img + ph[i].p_vaddr, ph[i].p_filesz, (off_t)ph[i].p_offset) != (ssize_t)ph[i].p_filesz) {
            fail(err, errlen, "cannot read a segment of %s", path);
            goto out;
        }
    }

    o = calloc(1, sizeof *o);
    if (!o) goto out;
    o->path = strdup(path);
    o->base = img;
    for (int i = 0; i < eh.e_phnum && o->nseg < 8; i++)
        if (ph[i].p_type == PT_LOAD) {
            o->seg[o->nseg].vaddr = ph[i].p_vaddr; o->seg[o->nseg].memsz = ph[i].p_memsz;
            o->seg[o->nseg].offset = ph[i].p_offset; o->seg[o->nseg].flags = ph[i].p_flags;
            o->nseg++;
        }
    o->span = span;
    const Rela *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltrelsz = 0;
    for (const Dyn *d = (const Dyn *)(img + dynph->p_vaddr); d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_HASH:         o->hash = (const uint32_t *)(img + d->d_val); break;
        case DT_STRTAB:       o->strtab = (const char *)(img + d->d_val); break;
        case DT_SYMTAB:       o->symtab = (const Sym *)(img + d->d_val); break;
        case DT_RELA:         rela = (const Rela *)(img + d->d_val); break;
        case DT_RELASZ:       relasz = d->d_val; break;
        case DT_JMPREL:       jmprel = (const Rela *)(img + d->d_val); break;
        case DT_PLTRELSZ:     pltrelsz = d->d_val; break;
        case DT_PLTREL:       if (d->d_val != DT_RELA) { fail(err, errlen, "%s uses REL PLT relocations", path); goto bad; } break;
        case DT_TEXTREL:      fail(err, errlen, "%s has text relocations", path); goto bad;
        case DT_INIT:         o->init = d->d_val; break;
        case DT_FINI:         o->fini = d->d_val; break;
        case DT_INIT_ARRAY:   o->init_array = d->d_val; break;
        case DT_INIT_ARRAYSZ: o->init_arraysz = d->d_val; break;
        case DT_FINI_ARRAY:   o->fini_array = d->d_val; break;
        case DT_FINI_ARRAYSZ: o->fini_arraysz = d->d_val; break;
        default: break;
        }
    }
    if (!o->strtab || !o->symtab || !o->hash) { fail(err, errlen, "%s lacks DT_STRTAB/DT_SYMTAB/DT_HASH", path); goto bad; }
    if (!relocate(o, rela, relasz / sizeof(Rela), resolve, err, errlen)) goto bad;
    if (!relocate(o, jmprel, pltrelsz / sizeof(Rela), resolve, err, errlen)) goto bad;
    free(ph);
    close(fd);
    return o;

bad:
    free(o->path);
    free(o);
    o = NULL;
out:
    if (img != MAP_FAILED) munmap(img, span);
    free(ph);
    close(fd);
    return NULL;
}

void elf_run_init(const struct elfobj *o, int argc, char **argv, char **envp)
{
    typedef void (*initfn)(int, char **, char **);
    if (o->init) ((initfn)(o->base + o->init))(argc, argv, envp);
    const uint64_t *arr = (const uint64_t *)(o->base + o->init_array);
    for (size_t i = 0; o->init_array && i < o->init_arraysz / 8; i++)
        if (arr[i] && arr[i] != (uint64_t)-1) ((initfn)(uintptr_t)arr[i])(argc, argv, envp);
}

void elf_run_fini(const struct elfobj *o)
{
    typedef void (*finifn)(void);
    const uint64_t *arr = (const uint64_t *)(o->base + o->fini_array);
    for (size_t i = o->fini_array ? o->fini_arraysz / 8 : 0; i-- > 0;)
        if (arr[i] && arr[i] != (uint64_t)-1) ((finifn)(uintptr_t)arr[i])();
    if (o->fini) ((finifn)(o->base + o->fini))();
}

const char *elf_path(const struct elfobj *o) { return o->path; }
uintptr_t elf_base(const struct elfobj *o) { return (uintptr_t)o->base; }
size_t elf_span(const struct elfobj *o) { return o->span; }
int elf_segment(const struct elfobj *o, int i, uint64_t *vaddr, uint64_t *memsz, uint64_t *offset, uint32_t *flags)
{
    if (i < 0 || i >= o->nseg) return 0;
    *vaddr = o->seg[i].vaddr; *memsz = o->seg[i].memsz; *offset = o->seg[i].offset; *flags = o->seg[i].flags;
    return 1;
}

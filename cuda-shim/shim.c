/*
 * cuda-shim glibc shim (see shim.h).
 *
 * Covers the 248 glibc imports of NVIDIA's libcuda 615.78.08, listed from its dynamic symbol table (C0 report).
 *   - Pass-through: same ABI and semantics on macOS x86_64 (string, math, stdio, malloc).
 *   - Translated: struct layouts (stat, dirent, utsname, sysinfo, rlimit), flag and number spaces (open/mmap/fcntl
 *     flags, sysconf names, clock ids, signals, rlimit resources), errno values, and glibc pthread/sem objects. Those
 *     have glibc sizes inside libcuda's own structures, so each holds a pointer to a separately allocated macOS
 *     object. That is the approach FreeBSD's libc6-shim takes; no code is taken from it.
 *   - Refused and logged: process creation (fork/exec/posix_spawn: libcuda would try to run Linux helpers such as
 *     nvidia-modprobe), sockets (MPS), eventfd/timerfd, raw syscalls other than gettid.
 *   - Device nodes and ioctl: not implemented yet (phase 2: /dev/nvidia* -> NVRM IOKit user client). open() of them
 *     fails with ENOENT, as on a Linux box without the driver.
 * Anything not in the table resolves to a trap stub that names the import and aborts.
 */
#include "shim.h"

#include <crt_externs.h>
#include <ctype.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* --- tracing ---------------------------------------------------------------------------------------------------- */

static int gTrace = -1;
int shim_trace(void)
{
    if (gTrace < 0) { const char *e = getenv("CUDASHIM_TRACE"); gTrace = e && (*e == '1' || *e == '2'); }
    return gTrace;
}
#define TRACE(...) do { if (shim_trace()) { fprintf(stderr, "cuda-shim: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define NOTE(...)  do { fprintf(stderr, "cuda-shim: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* --- errno ------------------------------------------------------------------------------------------------------ */

static _Thread_local int lx_errno_val;

static int errno_to_linux(int e)
{
    if (e == 0) return 0;
    if (e >= 1 && e <= 34 && e != 11) return e;   /* EPERM..ERANGE share numbers, except 11 */
    switch (e) {
    case EDEADLK: return 35;       case EAGAIN: return 11;        case EINPROGRESS: return 115;
    case EALREADY: return 114;     case ENOTSOCK: return 88;      case EDESTADDRREQ: return 89;
    case EMSGSIZE: return 90;      case EPROTOTYPE: return 91;    case ENOPROTOOPT: return 92;
    case EPROTONOSUPPORT: return 93; case ENOTSUP: return 95;     case EAFNOSUPPORT: return 97;
    case EADDRINUSE: return 98;    case EADDRNOTAVAIL: return 99; case ENETDOWN: return 100;
    case ECONNRESET: return 104;   case ENOBUFS: return 105;      case EISCONN: return 106;
    case ENOTCONN: return 107;     case ETIMEDOUT: return 110;    case ECONNREFUSED: return 111;
    case ELOOP: return 40;         case ENAMETOOLONG: return 36;  case ENOTEMPTY: return 39;
    case ENOSYS: return 38;        case ECANCELED: return 125;    case EOVERFLOW: return 75;
    case ENOLCK: return 37;        case EILSEQ: return 84;
    default: return 5;             /* EIO */
    }
}
static int errno_from_linux(int e)
{
    if (e >= 1 && e <= 34 && e != 11) return e;
    switch (e) {
    case 11: return EAGAIN;  case 35: return EDEADLK;     case 36: return ENAMETOOLONG; case 38: return ENOSYS;
    case 39: return ENOTEMPTY; case 40: return ELOOP;     case 75: return EOVERFLOW;    case 95: return ENOTSUP;
    case 110: return ETIMEDOUT; case 111: return ECONNREFUSED; case 115: return EINPROGRESS;
    default: return EIO;
    }
}
static int *lx___errno_location(void) { return &lx_errno_val; }
static void set_lx_errno_at(const char *fn, int linux_errno) { lx_errno_val = linux_errno; TRACE("%s fails, Linux errno %d", fn, linux_errno); }
#define set_lx_errno(e) set_lx_errno_at(__func__, (e))
/* After a failing macOS call: copy its errno into the Linux errno. Returns r unchanged. */
#define LXR(r) ({ __typeof__(r) _r = (r); if (_r == (__typeof__(r))-1) { lx_errno_val = errno_to_linux(errno);                      TRACE("%s failed, Linux errno %d", __func__, lx_errno_val); } _r; })
#define LXP(p) ({ __typeof__(p) _p = (p); if (!_p) { lx_errno_val = errno_to_linux(errno);                      TRACE("%s failed, Linux errno %d", __func__, lx_errno_val); } _p; })
static int fail_lx_at(const char *fn, int linux_errno)
{
    lx_errno_val = linux_errno;
    TRACE("%s fails, Linux errno %d", fn, linux_errno);
    return -1;
}
#define fail_lx(e) fail_lx_at(__func__, (e))

/* --- stdio data symbols, getopt ------------------------------------------------------------------------------- */

static FILE *lx_stdout_var, *lx_stderr_var;
static char *lx_optarg_var;
static int lx_optind_var = 1;

static int lx_getopt_long(int argc, char *const *argv, const char *opts, const struct option *lo, int *idx)
{
    optind = lx_optind_var;            /* struct option has the same layout in glibc and macOS */
    int r = getopt_long(argc, argv, opts, lo, idx);
    lx_optind_var = optind;
    lx_optarg_var = optarg;
    return r;
}

/* --- atexit / atfork / assert ------------------------------------------------------------------------------------ */

struct atexit_ent { void (*fn)(void *); void *arg; void *dso; };
static struct atexit_ent *gAtexit;
static size_t gAtexitN, gAtexitCap;
static pthread_mutex_t gAtexitLock = PTHREAD_MUTEX_INITIALIZER;

static int lx___cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    pthread_mutex_lock(&gAtexitLock);
    if (gAtexitN == gAtexitCap) {
        size_t cap = gAtexitCap ? gAtexitCap * 2 : 64;
        struct atexit_ent *n = realloc(gAtexit, cap * sizeof *n);
        if (!n) { pthread_mutex_unlock(&gAtexitLock); return -1; }
        gAtexit = n; gAtexitCap = cap;
    }
    gAtexit[gAtexitN++] = (struct atexit_ent){ fn, arg, dso };
    pthread_mutex_unlock(&gAtexitLock);
    return 0;
}
static void lx___cxa_finalize(void *dso)
{
    pthread_mutex_lock(&gAtexitLock);
    for (size_t i = gAtexitN; i-- > 0;) {
        if (!gAtexit[i].fn || (dso && gAtexit[i].dso != dso)) continue;
        struct atexit_ent e = gAtexit[i];
        gAtexit[i].fn = NULL;
        pthread_mutex_unlock(&gAtexitLock);
        e.fn(e.arg);
        pthread_mutex_lock(&gAtexitLock);
    }
    pthread_mutex_unlock(&gAtexitLock);
}
static void run_all_atexit(void) { lx___cxa_finalize(NULL); }

static int lx___register_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void), void *dso)
{
    (void)dso;
    return pthread_atfork(prepare, parent, child);
}
static void lx___assert_fail(const char *expr, const char *file, unsigned line, const char *func)
{
    fprintf(stderr, "cuda-shim: assertion in loaded library: %s (%s:%u %s)\n", expr, file, line, func ? func : "");
    abort();
}

/* --- files: flags, virtual paths, stat, dirent -------------------------------------------------------------------- */

/* eventfd emulation state (see lx_eventfd) */
enum { kMaxEvFd = 4096 };
static int gEvPeer[kMaxEvFd];             /* peer fd + 1; 0 = not an eventfd */
static unsigned char gEvFlags[kMaxEvFd];  /* 1 = non-blocking (user), 2 = semaphore */
static int is_evfd(int fd) { return fd >= 0 && fd < kMaxEvFd && gEvPeer[fd]; }

static int open_flags_from_linux(int f)
{
    int m = f & 3;
    if (f & 0100) m |= O_CREAT;       if (f & 0200) m |= O_EXCL;     if (f & 0400) m |= O_NOCTTY;
    if (f & 01000) m |= O_TRUNC;      if (f & 02000) m |= O_APPEND;  if (f & 04000) m |= O_NONBLOCK;
    if (f & 0200000) m |= O_DIRECTORY; if (f & 0400000) m |= O_NOFOLLOW; if (f & 02000000) m |= O_CLOEXEC;
    return m;                          /* O_LARGEFILE, O_DIRECT, O_NOATIME have no macOS meaning */
}

/* Paths that only exist on Linux. Device nodes come in phase 2; until then they are absent, like a Linux box without
 * the NVIDIA kernel module. */
static int linux_only_path(const char *p)
{
    return p && (!strncmp(p, "/proc/", 6) || !strncmp(p, "/sys/", 5) || !strncmp(p, "/dev/nvidia", 11) ||
                 !strncmp(p, "/run/nvidia", 11));
}

/* Synthesized /proc files: returns malloc'd content, or NULL if the path is not one we provide. */
/* macOS sysctl CPU feature names -> Linux /proc/cpuinfo flag names */
static const char *linux_flag(const char *mac)
{
    static const char *const map[][2] = {
        { "sse3", "pni" }, { "sse4.1", "sse4_1" }, { "sse4.2", "sse4_2" }, { "clfsh", "clflush" }, { "htt", "ht" },
        { "mon", "monitor" }, { "dscpl", "ds_cpl" }, { "dtse64", "dtes64" }, { "tpr", "xtpr" }, { "avx1.0", "avx" },
        { "seglim64", NULL }, { "fpu_csds", NULL }, { "em64t", "lm" }, { "xd", "nx" }, { "lahf", "lahf_lm" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcmp(mac, map[i][0])) return map[i][1];
    return mac;
}
static char *proc_cpuinfo(void)
{
    char brand[128] = "x86_64 CPU", raw[3072] = "", part[1024];
    size_t sz = sizeof brand;
    sysctlbyname("machdep.cpu.brand_string", brand, &sz, NULL, 0);
    static const char *const keys[] = { "machdep.cpu.features", "machdep.cpu.leaf7_features", "machdep.cpu.extfeatures" };
    for (size_t k = 0; k < 3; k++) {
        sz = sizeof part;
        if (sysctlbyname(keys[k], part, &sz, NULL, 0) == 0) { strlcat(raw, " ", sizeof raw); strlcat(raw, part, sizeof raw); }
    }
    for (char *p = raw; *p; p++) *p = (char)tolower((unsigned char)*p);
    char flags[3200] = "fpu";
    int has_lm = 0;
    for (char *save = NULL, *tok = strtok_r(raw, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        const char *f = linux_flag(tok);
        if (!f || !strcmp(f, "fpu")) continue;
        if (!strcmp(f, "lm")) has_lm = 1;
        strlcat(flags, " ", sizeof flags); strlcat(flags, f, sizeof flags);
    }
    if (!has_lm) strlcat(flags, " syscall nx lm", sizeof flags);   /* every x86_64 CPU; Rosetta omits extfeatures */
    strlcat(flags, " constant_tsc nonstop_tsc", sizeof flags);
    int64_t family = 6, model = 0, stepping = 0, hz = 0;
    sz = sizeof family;   sysctlbyname("machdep.cpu.family", &family, &sz, NULL, 0);
    sz = sizeof model;    sysctlbyname("machdep.cpu.model", &model, &sz, NULL, 0);
    sz = sizeof stepping; sysctlbyname("machdep.cpu.stepping", &stepping, &sz, NULL, 0);
    sz = sizeof hz;       sysctlbyname("hw.cpufrequency", &hz, &sz, NULL, 0);
    const long n = sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1;
    size_t cap = (size_t)n * (sizeof flags + 768), len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    for (long i = 0; i < n; i++)
        len += (size_t)snprintf(out + len, cap - len,
            "processor\t: %ld\nvendor_id\t: GenuineIntel\ncpu family\t: %lld\nmodel\t\t: %lld\nmodel name\t: %s\n"
            "stepping\t: %lld\ncpu MHz\t\t: %.3f\ncache size\t: 8192 KB\nphysical id\t: 0\nsiblings\t: %ld\n"
            "core id\t\t: %ld\ncpu cores\t: %ld\napicid\t\t: %ld\ninitial apicid\t: %ld\nfpu\t\t: yes\n"
            "fpu_exception\t: yes\ncpuid level\t: 13\nwp\t\t: yes\nflags\t\t: %s\nbogomips\t: %.2f\n"
            "clflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: 39 bits physical, 48 bits virtual\n"
            "power management:\n\n",
            i, (long long)family, (long long)model, brand, (long long)stepping, hz / 1e6, n, i, n, i, i, flags,
            hz / 5e5);
    return out;
}
static char *proc_self_maps(void);
static char *virtual_file(const char *p)
{
    if (!strcmp(p, "/proc/sys/vm/mmap_min_addr")) return strdup("65536\n");
    if (!strcmp(p, "/proc/cpuinfo")) return proc_cpuinfo();
    if (!strcmp(p, "/proc/self/maps")) return proc_self_maps();
    return NULL;
}
/* Content is served from an unlinked temporary file so it behaves like a real fd (read, lseek, fstat). */
static int virtual_fd(const char *path)
{
    char *c = virtual_file(path);
    if (!c) return -1;
    char tmpl[] = "/tmp/cuda-shim-proc.XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        unlink(tmpl);
        size_t n = strlen(c);
        if (write(fd, c, n) != (ssize_t)n) { close(fd); fd = -1; } else lseek(fd, 0, SEEK_SET);
    }
    free(c);
    TRACE("open %s -> virtual file, fd %d", path, fd);
    return fd;
}

static int lx_open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & 0100) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    int vfd = path ? virtual_fd(path) : -1;
    if (vfd >= 0) return vfd;
    /* thread names: libcuda writes "cuda0000..." to /proc/self/task/<tid>/comm; accept and discard */
    if (path && !strncmp(path, "/proc/", 6) && strstr(path, "/task/") && strstr(path, "/comm") && (flags & 3)) {
        TRACE("open %s -> /dev/null (thread name)", path);
        return LXR(open("/dev/null", O_WRONLY | O_CLOEXEC));
    }
    if (linux_only_path(path)) { TRACE("open %s -> ENOENT (Linux-only path)", path); return fail_lx(2); }
    int r = open(path, open_flags_from_linux(flags), mode);
    TRACE("open %s flags 0%o -> %d", path, flags, r);
    return LXR(r);
}
static int lx_openat64(int dirfd, const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & 0100) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (linux_only_path(path)) { TRACE("openat %s -> ENOENT", path); return fail_lx(2); }
    return LXR(openat(dirfd == -100 ? AT_FDCWD : dirfd, path, open_flags_from_linux(flags), mode));
}
static FILE *lx_fopen(const char *path, const char *mode)
{
    int vfd = path ? virtual_fd(path) : -1;
    if (vfd >= 0) return fdopen(vfd, "r");
    if (path && !strncmp(path, "/proc/", 6) && strstr(path, "/task/") && strstr(path, "/comm") && mode && mode[0] != 'r') {
        TRACE("fopen %s -> /dev/null (thread name)", path);
        return LXP(fopen("/dev/null", "w"));
    }
    if (linux_only_path(path)) { TRACE("fopen %s -> ENOENT (Linux-only path)", path); set_lx_errno(2); return NULL; }
    FILE *f = fopen(path, mode);
    TRACE("fopen %s %s -> %p", path, mode, (void *)f);
    return LXP(f);
}
static FILE *lx_fdopen(int fd, const char *mode) { return LXP(fdopen(fd, mode)); }
static ssize_t copy_link(const char *s, char *buf, size_t n)
{
    size_t len = strlen(s);
    if (len > n) len = n;
    memcpy(buf, s, len);               /* readlink does not NUL-terminate */
    return (ssize_t)len;
}
/* Linux /proc link semantics, as the reference strace shows libcuda walking them: "/proc" and "/proc/<pid>" are not
 * links (EINVAL), "/proc/self" -> "<pid>", "/proc/self|<pid>/exe" -> the executable. */
static ssize_t lx_readlink(const char *path, char *buf, size_t n)
{
    if (path && (!strcmp(path, "/proc") || !strncmp(path, "/proc/", 6))) {
        char pid[16], self_exe[48];
        snprintf(pid, sizeof pid, "%d", getpid());
        snprintf(self_exe, sizeof self_exe, "/proc/%d/exe", getpid());
        if (!strcmp(path, "/proc/self")) return copy_link(pid, buf, n);
        if (!strcmp(path, "/proc/self/exe") || !strcmp(path, self_exe)) {
            char exe[PATH_MAX]; uint32_t sz = sizeof exe;
            if (_NSGetExecutablePath(exe, &sz) != 0) return fail_lx(36);
            char real[PATH_MAX];
            return copy_link(realpath(exe, real) ? real : exe, buf, n);
        }
        if (!strcmp(path, "/proc") || !strcmp(path + 6, pid)) return fail_lx(22);
        TRACE("readlink %s -> ENOENT", path);
        return fail_lx(2);
    }
    return LXR(readlink(path, buf, n));
}
static int lx_access(const char *path, int mode)
{
    if (linux_only_path(path)) { TRACE("access %s -> ENOENT", path); return fail_lx(2); }
    return LXR(access(path, mode));
}
static int lx_faccessat(int dirfd, const char *path, int mode, int flags)
{
    if (linux_only_path(path)) { TRACE("faccessat %s -> ENOENT", path); return fail_lx(2); }
    int mf = (flags & 0x200) ? AT_EACCESS : 0;
    return LXR(faccessat(dirfd == -100 ? AT_FDCWD : dirfd, path, mode, mf));
}

struct lx_stat { uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad0; uint64_t rdev; int64_t size, blksize, blocks;
                 int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; int64_t reserved[3]; };
_Static_assert(sizeof(struct lx_stat) == 144, "Linux x86_64 struct stat is 144 bytes");

static void stat_to_linux(const struct stat *s, struct lx_stat *l)
{
    memset(l, 0, sizeof *l);
    l->dev = (uint64_t)s->st_dev; l->ino = s->st_ino; l->nlink = s->st_nlink; l->mode = s->st_mode;
    l->uid = s->st_uid; l->gid = s->st_gid; l->rdev = (uint64_t)s->st_rdev; l->size = s->st_size;
    l->blksize = s->st_blksize; l->blocks = s->st_blocks;
    l->atime = s->st_atimespec.tv_sec; l->atime_ns = s->st_atimespec.tv_nsec;
    l->mtime = s->st_mtimespec.tv_sec; l->mtime_ns = s->st_mtimespec.tv_nsec;
    l->ctime = s->st_ctimespec.tv_sec; l->ctime_ns = s->st_ctimespec.tv_nsec;
}
static int lx___xstat(int ver, const char *path, struct lx_stat *out)
{
    (void)ver;
    if (linux_only_path(path)) { TRACE("stat %s -> ENOENT", path); return fail_lx(2); }
    struct stat s;
    if (stat(path, &s) != 0) return fail_lx(errno_to_linux(errno));
    stat_to_linux(&s, out);
    return 0;
}
static int lx___fxstat(int ver, int fd, struct lx_stat *out)
{
    (void)ver;
    struct stat s;
    if (fstat(fd, &s) != 0) return fail_lx(errno_to_linux(errno));
    stat_to_linux(&s, out);
    return 0;
}
static int lx___xmknod(int ver, const char *path, unsigned mode, void *dev)
{
    (void)ver; (void)mode; (void)dev;
    NOTE("refused: mknod %s (no Linux device nodes on macOS)", path);
    return fail_lx(1);
}

struct lx_dirent { uint64_t ino; int64_t off; uint16_t reclen; uint8_t type; char name[256]; };
static _Thread_local struct lx_dirent tDirent;

static DIR *lx_opendir(const char *path)
{
    if (linux_only_path(path)) { TRACE("opendir %s -> ENOENT", path); set_lx_errno(2); return NULL; }
    return LXP(opendir(path));
}
static void dirent_to_linux(const struct dirent *d, struct lx_dirent *l)
{
    memset(l, 0, offsetof(struct lx_dirent, name));
    l->ino = d->d_ino; l->reclen = sizeof *l; l->type = d->d_type;   /* DT_* values match */
    strlcpy(l->name, d->d_name, sizeof l->name);
}
static struct lx_dirent *lx_readdir(DIR *d)
{
    errno = 0;
    struct dirent *e = readdir(d);
    if (!e) { if (errno) set_lx_errno(errno_to_linux(errno)); return NULL; }
    dirent_to_linux(e, &tDirent);
    return &tDirent;
}
static int lx_alphasort64(const struct lx_dirent **a, const struct lx_dirent **b)
{
    return strcoll((*a)->name, (*b)->name);
}
static int lx_scandir64(const char *path, struct lx_dirent ***out, int (*filter)(const struct lx_dirent *),
                        int (*cmp)(const struct lx_dirent **, const struct lx_dirent **))
{
    DIR *d = lx_opendir(path);
    if (!d) return -1;
    struct lx_dirent **v = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        struct lx_dirent tmp;
        dirent_to_linux(e, &tmp);
        if (filter && !filter(&tmp)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            struct lx_dirent **nv = realloc(v, cap * sizeof *nv);
            if (!nv) break;
            v = nv;
        }
        struct lx_dirent *c = malloc(sizeof *c);
        if (!c) break;
        *c = tmp;
        v[n++] = c;
    }
    closedir(d);
    if (cmp && n) qsort(v, n, sizeof *v, (int (*)(const void *, const void *))cmp);
    *out = v;
    return (int)n;
}
static int lx_nftw(const char *path, void *fn, int fds, int flags)
{
    (void)fn; (void)fds; (void)flags;
    NOTE("not implemented: nftw %s -> ENOSYS", path);
    return fail_lx(38);
}

static int lx_fcntl(int fd, int cmd, ...)
{
    va_list ap; va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    switch (cmd) {
    case 0: return LXR(fcntl(fd, F_DUPFD, (int)arg));
    case 1: return LXR(fcntl(fd, F_GETFD));
    case 2: return LXR(fcntl(fd, F_SETFD, (int)arg));       /* FD_CLOEXEC is 1 on both */
    case 3: {
        int m = fcntl(fd, F_GETFL);
        if (m < 0) return fail_lx(errno_to_linux(errno));
        int l = m & 3;
        if (m & O_APPEND) l |= 02000; if (m & O_NONBLOCK) l |= 04000;
        return l;
    }
    case 4:
        if (is_evfd(fd)) {             /* the emulated eventfd stays non-blocking inside; remember the user's choice */
            gEvFlags[fd] = (unsigned char)((gEvFlags[fd] & ~1) | ((arg & 04000) ? 1 : 0));
            return 0;
        }
        return LXR(fcntl(fd, F_SETFL, open_flags_from_linux((int)arg) & (O_APPEND | O_NONBLOCK)));
    case 1030: return LXR(fcntl(fd, F_DUPFD_CLOEXEC, (int)arg));
    default:
        NOTE("not implemented: fcntl cmd %d -> EINVAL", cmd);
        return fail_lx(22);
    }
}
static int lx_dup3(int a, int b, int flags)
{
    if (a == b) return fail_lx(22);
    int r = dup2(a, b);
    if (r >= 0 && (flags & 02000000)) fcntl(r, F_SETFD, FD_CLOEXEC);
    return LXR(r);
}
static int lx_shm_open(const char *name, int flags, mode_t mode) { return LXR(shm_open(name, open_flags_from_linux(flags), mode)); }
static int lx_ioctl(int fd, unsigned long req, void *arg)
{
    (void)arg;
    TRACE("ioctl fd %d req 0x%lx -> ENOTTY (device shim not implemented yet)", fd, req);
    return fail_lx(25);
}

/* errno-translating wrappers for plain calls */
/* eventfd emulation. libcuda's worker thread waits on an eventfd with poll(). Linux gives one fd that is both read
 * and written; here it is one end (A) of a socketpair, and writes to A are redirected to the peer (B) so they become
 * readable on A. read() drains every queued 8-byte value and returns their sum (EFD_SEMAPHORE: returns 1). A is kept
 * non-blocking internally; a blocking read polls first. */
static int lx_eventfd(unsigned init, int flags)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return fail_lx(errno_to_linux(errno));
    if (sv[0] >= kMaxEvFd) { close(sv[0]); close(sv[1]); return fail_lx(24); }
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    if (flags & 02000000) { fcntl(sv[0], F_SETFD, FD_CLOEXEC); fcntl(sv[1], F_SETFD, FD_CLOEXEC); }
    gEvPeer[sv[0]] = sv[1] + 1;
    gEvFlags[sv[0]] = (unsigned char)(((flags & 04000) ? 1 : 0) | ((flags & 1) ? 2 : 0));
    if (init) { uint64_t v = init; (void)!write(sv[1], &v, 8); }
    TRACE("eventfd -> fd %d (socketpair emulation)", sv[0]);
    return sv[0];
}
static ssize_t ev_read(int fd, void *buf, size_t n)
{
    if (n < 8) return fail_lx(22);
    for (;;) {
        uint64_t sum = 0, v;
        while (read(fd, &v, 8) == 8) {
            if (gEvFlags[fd] & 2) {                                  /* semaphore: take one unit */
                if (v > 1) { uint64_t rest = v - 1; (void)!write(gEvPeer[fd] - 1, &rest, 8); }
                sum = 1;
                break;
            }
            sum += v;
        }
        if (sum) { memcpy(buf, &sum, 8); return 8; }
        if (gEvFlags[fd] & 1) return fail_lx(11);                    /* EAGAIN */
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, -1) < 0 && errno != EINTR) return fail_lx(errno_to_linux(errno));
    }
}
static ssize_t ev_write(int fd, const void *buf, size_t n)
{
    if (n < 8) return fail_lx(22);
    uint64_t v;
    memcpy(&v, buf, 8);
    if (v == ~0ULL) return fail_lx(22);
    if (write(gEvPeer[fd] - 1, &v, 8) != 8) return fail_lx(errno_to_linux(errno));
    return 8;
}

static ssize_t lx_read(int fd, void *b, size_t n) { return is_evfd(fd) ? ev_read(fd, b, n) : LXR(read(fd, b, n)); }
static ssize_t lx_write(int fd, const void *b, size_t n) { return is_evfd(fd) ? ev_write(fd, b, n) : LXR(write(fd, b, n)); }
static int lx_close(int fd)
{
    if (is_evfd(fd)) { close(gEvPeer[fd] - 1); gEvPeer[fd] = 0; gEvFlags[fd] = 0; }
    return LXR(close(fd));
}
static int lx_pipe2(int fds[2], int flags)
{
    if (pipe(fds) != 0) return fail_lx(errno_to_linux(errno));
    for (int i = 0; i < 2; i++) {
        if (flags & 02000000) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        if (flags & 04000) fcntl(fds[i], F_SETFL, O_NONBLOCK);
    }
    return 0;
}
static int lx_accept4(int s, struct sockaddr *a, socklen_t *l, int flags)
{
    int fd = accept(s, a, l);
    if (fd < 0) return fail_lx(errno_to_linux(errno));
    if (flags & 02000000) fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (flags & 04000) fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}
static int lx_sched_getcpu(void) { return 0; }   /* macOS does not expose the current CPU */
/* macOS has no thread affinity: report every CPU as allowed and accept (ignore) affinity requests */
static int lx_pthread_getaffinity_np(pthread_t t, size_t sz, unsigned char *set)
{
    (void)t;
    memset(set, 0, sz);
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (long i = 0; i < n && (size_t)(i / 8) < sz; i++) set[i / 8] |= (unsigned char)(1u << (i % 8));
    return 0;
}
static int lx_pthread_setaffinity_np(pthread_t t, size_t sz, const void *set) { (void)t; (void)sz; (void)set; return 0; }
static off_t lx_lseek(int fd, off_t o, int w) { return LXR(lseek(fd, o, w)); }
static int lx_mkdir(const char *p, mode_t m) { return LXR(mkdir(p, m)); }
static int lx_rmdir(const char *p) { return LXR(rmdir(p)); }
static int lx_unlink(const char *p) { return LXR(unlink(p)); }
static int lx_remove(const char *p) { return LXR(remove(p)); }
static int lx_chmod(const char *p, mode_t m) { return LXR(chmod(p, m)); }
static int lx_fchmod(int fd, mode_t m) { return LXR(fchmod(fd, m)); }
static int lx_chown(const char *p, uid_t u, gid_t g) { return LXR(chown(p, u, g)); }
static int lx_symlink(const char *a, const char *b) { return LXR(symlink(a, b)); }
static int lx_dup(int fd) { return LXR(dup(fd)); }
static int lx_dup2(int a, int b) { return LXR(dup2(a, b)); }
static int lx_ftruncate(int fd, off_t n) { return LXR(ftruncate(fd, n)); }
static int lx_flock(int fd, int op) { return LXR(flock(fd, op)); }
static int lx_mkfifo(const char *p, mode_t m) { return LXR(mkfifo(p, m)); }
static int lx_pipe(int fds[2]) { return LXR(pipe(fds)); }
static int lx_poll(struct pollfd *f, nfds_t n, int t) { return LXR(poll(f, n, t)); }
static int lx_closedir(DIR *d) { return LXR(closedir(d)); }
static char *lx_realpath(const char *p, char *r) { return LXP(realpath(p, r)); }
static int lx_shm_unlink(const char *n) { return LXR(shm_unlink(n)); }
static void *lx_memalign(size_t align, size_t size)
{
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    int r = posix_memalign(&p, align, size);
    if (r) { set_lx_errno(errno_to_linux(r)); return NULL; }
    return p;
}

/* --- memory --------------------------------------------------------------------------------------------------------- */

static void *lx_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    int m = flags & 3;                                        /* MAP_SHARED 1, MAP_PRIVATE 2 */
    if (flags & 0x10) m |= MAP_FIXED;
    if (flags & 0x20) m |= MAP_ANON;
    if (flags & 0x4000) m |= MAP_NORESERVE;
    const int known = 3 | 0x10 | 0x20 | 0x4000 | 0x8000 | 0x2000 | 0x100 | 0x40000 | 0x100000;
    if (flags & ~known) NOTE("mmap: ignoring unknown Linux flags 0x%x", flags & ~known);
    const int noreplace = (flags & 0x100000) != 0;           /* MAP_FIXED_NOREPLACE: use as hint, check result */
    if (flags & 0x40) { NOTE("mmap: MAP_32BIT not supported -> ENOMEM"); set_lx_errno(12); return MAP_FAILED; }
    void *r = mmap(addr, len, prot, m, (flags & 0x20) ? -1 : fd, off);
    TRACE("mmap %p len 0x%zx prot %d flags 0x%x fd %d -> %p", addr, len, prot, flags, fd, r);
    if (r == MAP_FAILED) { set_lx_errno(errno_to_linux(errno)); return MAP_FAILED; }
    if (noreplace && r != addr) { munmap(r, len); set_lx_errno(17); return MAP_FAILED; }
    return r;
}
static int lx_munmap(void *a, size_t n) { return LXR(munmap(a, n)); }
static int lx_mprotect(void *a, size_t n, int p) { return LXR(mprotect(a, n, p)); }
static int lx_mlock(const void *a, size_t n) { return LXR(mlock(a, n)); }
static int lx_munlock(const void *a, size_t n) { return LXR(munlock(a, n)); }
static int lx_madvise(void *a, size_t n, int adv)
{
    if (adv >= 0 && adv <= 4) return LXR(madvise(a, n, adv));   /* NORMAL..DONTNEED match */
    TRACE("madvise advice %d ignored", adv);                     /* DONTFORK, HUGEPAGE, ... have no macOS meaning */
    return 0;
}
static int lx_msync(void *a, size_t n, int f)
{
    int m = 0;
    if (f & 1) m |= MS_ASYNC; if (f & 2) m |= MS_INVALIDATE; if (f & 4) m |= MS_SYNC;
    return LXR(msync(a, n, m));
}
static void *lx_mremap(void *old, size_t oldsz, size_t newsz, int flags, ...)
{
    /* Only MREMAP_MAYMOVE of private anonymous memory can be emulated (map, copy, unmap). */
    if (!(flags & 1) || (flags & ~1)) { NOTE("mremap flags 0x%x not supported -> ENOMEM", flags); set_lx_errno(12); return MAP_FAILED; }
    void *n = mmap(NULL, newsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (n == MAP_FAILED) { set_lx_errno(12); return MAP_FAILED; }
    memcpy(n, old, oldsz < newsz ? oldsz : newsz);
    munmap(old, oldsz);
    TRACE("mremap emulated %zu -> %zu bytes (assumes private anonymous memory)", oldsz, newsz);
    return n;
}

/* --- system information ------------------------------------------------------------------------------------------ */

static long lx_sysconf(int name)
{
    switch (name) {
    case 0: return sysconf(_SC_ARG_MAX);       case 1: return sysconf(_SC_CHILD_MAX);
    case 2: return sysconf(_SC_CLK_TCK);       case 3: return sysconf(_SC_NGROUPS_MAX);
    case 4: return sysconf(_SC_OPEN_MAX);      case 30: return sysconf(_SC_PAGESIZE);
    case 83: return sysconf(_SC_NPROCESSORS_CONF); case 84: return sysconf(_SC_NPROCESSORS_ONLN);
    case 85: return sysconf(_SC_PHYS_PAGES);
    case 86: return sysconf(_SC_PHYS_PAGES) / 2;   /* _SC_AVPHYS_PAGES has no macOS equivalent; estimate */
    case 190: { int64_t v = 64; size_t sz = sizeof v; sysctlbyname("hw.cachelinesize", &v, &sz, NULL, 0); return (long)v; }
    default: NOTE("sysconf(%d) unknown -> EINVAL", name); set_lx_errno(22); return -1;
    }
}
static int lx_get_nprocs(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }

struct lx_sysinfo { long uptime; unsigned long loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
                    unsigned short procs, pad; unsigned long totalhigh, freehigh; unsigned mem_unit; char f[4]; };
_Static_assert(sizeof(struct lx_sysinfo) == 112, "Linux x86_64 struct sysinfo is 112 bytes");
static int lx_sysinfo(struct lx_sysinfo *s)
{
    memset(s, 0, sizeof *s);
    uint64_t mem = 0; size_t sz = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &sz, NULL, 0);
    struct timeval boot; sz = sizeof boot;
    if (sysctlbyname("kern.boottime", &boot, &sz, NULL, 0) == 0) s->uptime = time(NULL) - boot.tv_sec;
    s->totalram = mem; s->freeram = mem / 2; s->mem_unit = 1; s->procs = 1;
    return 0;
}

struct lx_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
static int lx_uname(struct lx_utsname *u)
{
    /* libcuda expects a Linux kernel; report a plausible one rather than Darwin. CUDASHIM_UNAME_RELEASE overrides. */
    memset(u, 0, sizeof *u);
    struct utsname m;
    uname(&m);
    const char *rel = getenv("CUDASHIM_UNAME_RELEASE");
    strlcpy(u->sysname, "Linux", sizeof u->sysname);
    strlcpy(u->nodename, m.nodename, sizeof u->nodename);
    strlcpy(u->release, rel ? rel : "6.8.0-cudashim", sizeof u->release);
    strlcpy(u->version, "#1 SMP cuda-shim", sizeof u->version);
    strlcpy(u->machine, "x86_64", sizeof u->machine);
    return 0;
}

static int rlimit_resource_from_linux(int r)
{
    switch (r) {
    case 0: return RLIMIT_CPU; case 1: return RLIMIT_FSIZE; case 2: return RLIMIT_DATA; case 3: return RLIMIT_STACK;
    case 4: return RLIMIT_CORE; case 6: return RLIMIT_NPROC; case 7: return RLIMIT_NOFILE; case 8: return RLIMIT_MEMLOCK;
    case 9: return RLIMIT_AS; default: return -1;
    }
}
static int lx_getrlimit(int res, uint64_t lim[2])
{
    int r = rlimit_resource_from_linux(res);
    if (r < 0) return fail_lx(22);
    struct rlimit rl;
    if (getrlimit(r, &rl) != 0) return fail_lx(errno_to_linux(errno));
    lim[0] = rl.rlim_cur == RLIM_INFINITY ? ~0ULL : rl.rlim_cur;
    lim[1] = rl.rlim_max == RLIM_INFINITY ? ~0ULL : rl.rlim_max;
    return 0;
}
static int lx_setrlimit(int res, const uint64_t lim[2])
{
    int r = rlimit_resource_from_linux(res);
    if (r < 0) return fail_lx(22);
    struct rlimit rl = { lim[0] == ~0ULL ? RLIM_INFINITY : lim[0], lim[1] == ~0ULL ? RLIM_INFINITY : lim[1] };
    return LXR(setrlimit(r, &rl));
}

static int lx_clock_gettime(int id, struct timespec *ts)
{
    clockid_t c;
    switch (id) {
    case 0: case 5: c = CLOCK_REALTIME; break;
    case 1: case 6: case 7: c = CLOCK_MONOTONIC; break;
    case 2: c = CLOCK_PROCESS_CPUTIME_ID; break;
    case 3: c = CLOCK_THREAD_CPUTIME_ID; break;
    case 4: c = CLOCK_MONOTONIC_RAW; break;
    default: return fail_lx(22);
    }
    return LXR(clock_gettime(c, ts));
}
static int sched_policy_from_linux(int p) { return p == 1 ? SCHED_FIFO : p == 2 ? SCHED_RR : SCHED_OTHER; }
static int lx_sched_get_priority_max(int p) { return sched_get_priority_max(sched_policy_from_linux(p)); }
static int lx_sched_get_priority_min(int p) { return sched_get_priority_min(sched_policy_from_linux(p)); }

static char *lx_strerror(int e) { return strerror(errno_from_linux(e)); }
static char *lx_strerror_r(int e, char *buf, size_t n)    /* GNU variant: returns the message */
{
    if (strerror_r(errno_from_linux(e), buf, n) != 0 && n) snprintf(buf, n, "Unknown error %d", e);
    return buf;
}

/* --- signals ------------------------------------------------------------------------------------------------------ */

static int signal_from_linux(int s)
{
    switch (s) {
    case 7: return SIGBUS; case 10: return SIGUSR1; case 12: return SIGUSR2; case 17: return SIGCHLD;
    case 18: return SIGCONT; case 19: return SIGSTOP; case 20: return SIGTSTP; case 23: return SIGURG; case 29: return SIGIO;
    default: return (s >= 0 && s <= 15 && s != 7 && s != 10 && s != 12) ? s : -1;
    }
}
static int lx_sigaction(int sig, const void *act, void *old)
{
    /* glibc struct sigaction (152 bytes) differs from macOS, and SA_SIGINFO handlers would receive a macOS
     * siginfo_t. Until a translating trampoline exists, report success without installing, and say so. */
    if (old) memset(old, 0, 152);
    if (act) NOTE("sigaction(%d) not installed (no siginfo translation yet)", sig);
    return 0;
}
static int lx_pthread_kill(pthread_t t, int sig)
{
    int s = signal_from_linux(sig);
    if (s < 0) return 22;
    return errno_to_linux(pthread_kill(t, s));
}

/* --- processes and IPC: refused ----------------------------------------------------------------------------------- */

static pid_t lx_fork(void) { NOTE("refused: fork"); return fail_lx(11); }
static int lx_execvp(const char *f, char *const argv[]) { (void)argv; NOTE("refused: execvp %s", f); return fail_lx(13); }
static int lx_execle(const char *p, const char *a, ...) { (void)a; NOTE("refused: execle %s", p); return fail_lx(13); }
static int lx_posix_spawn(pid_t *pid, const char *path, const void *fa, const void *attr, char *const argv[], char *const envp[])
{
    (void)pid; (void)fa; (void)attr; (void)argv; (void)envp;
    NOTE("refused: posix_spawn %s", path);
    return 13;
}
static int lx_spawn_fa_noop(void *fa, ...) { (void)fa; return 0; }
static pid_t lx_waitpid(pid_t p, int *st, int o) { return LXR(waitpid(p, st, o)); }

static int lx_enosys(void) { return fail_lx(38); }
static int lx_socket(int d, int t, int p) { TRACE("socket(%d,%d,%d) -> EAFNOSUPPORT (no IPC/MPS yet)", d, t, p); return fail_lx(97); }
static int lx_timerfd_create(int c, int f) { (void)c; (void)f; NOTE("not implemented: timerfd_create -> ENOSYS"); return fail_lx(38); }
static long lx_syscall(long n, ...)
{
    if (n == 186) { uint64_t tid; pthread_threadid_np(NULL, &tid); return (long)(uint32_t)tid; }   /* gettid */
    NOTE("not implemented: syscall %ld -> ENOSYS", n);
    return fail_lx(38);
}

/* --- pthread objects ------------------------------------------------------------------------------------------------
 * glibc objects live inside libcuda's structures at glibc sizes; the first 8 bytes hold a pointer to the macOS object,
 * created on first use (static initializers are all-zero in glibc). Attribute objects that are only 4 bytes in glibc
 * (mutexattr, condattr) store their settings as bits instead. */

struct lx_mutex { _Atomic(pthread_mutex_t *) m; int pad[2]; int kind; int pad2[5]; };   /* glibc: kind at offset 16 */
_Static_assert(sizeof(struct lx_mutex) == 40, "glibc pthread_mutex_t is 40 bytes");

static pthread_mutex_t *mutex_of(struct lx_mutex *x, unsigned attrbits)
{
    pthread_mutex_t *m = atomic_load(&x->m);
    if (m) return m;
    pthread_mutex_t *n = malloc(sizeof *n);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    int kind = attrbits ? (int)(attrbits & 3) : (x->kind & 3);   /* glibc kinds: 0 normal, 1 recursive, 2 errorcheck */
    pthread_mutexattr_settype(&a, kind == 1 ? PTHREAD_MUTEX_RECURSIVE : kind == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    if (attrbits & 0x20) pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(n, &a);
    pthread_mutexattr_destroy(&a);
    pthread_mutex_t *expected = NULL;
    if (!atomic_compare_exchange_strong(&x->m, &expected, n)) { pthread_mutex_destroy(n); free(n); return expected; }
    return n;
}
static int lx_pthread_mutexattr_init(unsigned *a) { *a = 0x80; return 0; }      /* 0x80: "initialised" marker */
static int lx_pthread_mutexattr_destroy(unsigned *a) { *a = 0; return 0; }
static int lx_pthread_mutexattr_settype(unsigned *a, int t) { if (t < 0 || t > 3) return 22; *a = (*a & ~3u) | (unsigned)(t == 3 ? 0 : t); return 0; }
static int lx_pthread_mutexattr_setpshared(unsigned *a, int s) { if (s) TRACE("process-shared mutex requested; macOS mutexes are process-private"); *a = (*a & ~0x10u) | (s ? 0x10 : 0); return 0; }
static int lx_pthread_mutexattr_setprotocol(unsigned *a, int p) { *a = (*a & ~0x60u) | (p == 1 ? 0x20 : p == 2 ? 0x40 : 0); return 0; }
static int lx_pthread_mutex_init(struct lx_mutex *x, const unsigned *a)
{
    memset(x, 0, sizeof *x);
    mutex_of(x, a ? (*a | 0x100) : 0);   /* | 0x100 so a zero-kind attribute still counts as "given" */
    return 0;
}
static int lx_pthread_mutex_destroy(struct lx_mutex *x)
{
    pthread_mutex_t *m = atomic_exchange(&x->m, NULL);
    if (m) { pthread_mutex_destroy(m); free(m); }
    return 0;
}
/* pthread functions return an error number instead of setting errno */
static int pt(int r) { return r ? errno_to_linux(r) : 0; }

static int lx_pthread_mutex_lock(struct lx_mutex *x) { return pt(pthread_mutex_lock(mutex_of(x, 0))); }
static int lx_pthread_mutex_trylock(struct lx_mutex *x) { return pt(pthread_mutex_trylock(mutex_of(x, 0))); }
static int lx_pthread_mutex_unlock(struct lx_mutex *x) { return pt(pthread_mutex_unlock(mutex_of(x, 0))); }

struct lx_cond { _Atomic(pthread_cond_t *) c; char pad[40]; };
_Static_assert(sizeof(struct lx_cond) == 48, "glibc pthread_cond_t is 48 bytes");
static pthread_cond_t *cond_of(struct lx_cond *x)
{
    pthread_cond_t *c = atomic_load(&x->c);
    if (c) return c;
    pthread_cond_t *n = malloc(sizeof *n);
    pthread_cond_init(n, NULL);
    pthread_cond_t *expected = NULL;
    if (!atomic_compare_exchange_strong(&x->c, &expected, n)) { pthread_cond_destroy(n); free(n); return expected; }
    return n;
}
static int lx_pthread_condattr_init(unsigned *a) { *a = 0; return 0; }
static int lx_pthread_condattr_setpshared(unsigned *a, int s) { if (s) TRACE("process-shared condvar requested; process-private on macOS"); *a = s ? 1 : 0; return 0; }
static int lx_pthread_cond_init(struct lx_cond *x, const unsigned *a) { (void)a; memset(x, 0, sizeof *x); cond_of(x); return 0; }
static int lx_pthread_cond_destroy(struct lx_cond *x)
{
    pthread_cond_t *c = atomic_exchange(&x->c, NULL);
    if (c) { pthread_cond_destroy(c); free(c); }
    return 0;
}
static int lx_pthread_cond_wait(struct lx_cond *c, struct lx_mutex *m) { return pt(pthread_cond_wait(cond_of(c), mutex_of(m, 0))); }
static int lx_pthread_cond_timedwait(struct lx_cond *c, struct lx_mutex *m, const struct timespec *t)
{
    return pt(pthread_cond_timedwait(cond_of(c), mutex_of(m, 0), t));   /* CLOCK_REALTIME absolute time on both */
}
static int lx_pthread_cond_signal(struct lx_cond *c) { return pt(pthread_cond_signal(cond_of(c))); }
static int lx_pthread_cond_broadcast(struct lx_cond *c) { return pt(pthread_cond_broadcast(cond_of(c))); }

struct lx_rwlock { _Atomic(pthread_rwlock_t *) l; char pad[48]; };
_Static_assert(sizeof(struct lx_rwlock) == 56, "glibc pthread_rwlock_t is 56 bytes");
static pthread_rwlock_t *rw_of(struct lx_rwlock *x)
{
    pthread_rwlock_t *l = atomic_load(&x->l);
    if (l) return l;
    pthread_rwlock_t *n = malloc(sizeof *n);
    pthread_rwlock_init(n, NULL);
    pthread_rwlock_t *expected = NULL;
    if (!atomic_compare_exchange_strong(&x->l, &expected, n)) { pthread_rwlock_destroy(n); free(n); return expected; }
    return n;
}
static int lx_pthread_rwlockattr_init(uint64_t *a) { *a = 0; return 0; }
static int lx_pthread_rwlockattr_destroy(uint64_t *a) { *a = 0; return 0; }
static int lx_pthread_rwlockattr_setpshared(uint64_t *a, int s) { *a = s ? 1 : 0; return 0; }
static int lx_pthread_rwlock_init(struct lx_rwlock *x, const uint64_t *a) { (void)a; memset(x, 0, sizeof *x); rw_of(x); return 0; }
static int lx_pthread_rwlock_destroy(struct lx_rwlock *x)
{
    pthread_rwlock_t *l = atomic_exchange(&x->l, NULL);
    if (l) { pthread_rwlock_destroy(l); free(l); }
    return 0;
}
static int lx_pthread_rwlock_rdlock(struct lx_rwlock *x) { return pt(pthread_rwlock_rdlock(rw_of(x))); }
static int lx_pthread_rwlock_wrlock(struct lx_rwlock *x) { return pt(pthread_rwlock_wrlock(rw_of(x))); }
static int lx_pthread_rwlock_trywrlock(struct lx_rwlock *x) { return pt(pthread_rwlock_trywrlock(rw_of(x))); }
static int lx_pthread_rwlock_unlock(struct lx_rwlock *x) { return pt(pthread_rwlock_unlock(rw_of(x))); }

/* glibc pthread_once_t is a 4-byte int, 0 = not run. 1 = running, 2 = done. */
static pthread_mutex_t gOnceLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gOnceCond = PTHREAD_COND_INITIALIZER;
static int lx_pthread_once(_Atomic int *once, void (*fn)(void))
{
    if (atomic_load(once) == 2) return 0;
    pthread_mutex_lock(&gOnceLock);
    while (atomic_load(once) == 1) pthread_cond_wait(&gOnceCond, &gOnceLock);
    if (atomic_load(once) == 2) { pthread_mutex_unlock(&gOnceLock); return 0; }
    atomic_store(once, 1);
    pthread_mutex_unlock(&gOnceLock);
    fn();
    pthread_mutex_lock(&gOnceLock);
    atomic_store(once, 2);
    pthread_cond_broadcast(&gOnceCond);
    pthread_mutex_unlock(&gOnceLock);
    return 0;
}

/* glibc pthread_key_t is unsigned int: take it as 32 bits so stale upper register halves never leak in */
static int lx_pthread_key_create(unsigned *k, void (*d)(void *))
{
    pthread_key_t mk;
    int r = pthread_key_create(&mk, d);
    if (!r) *k = (unsigned)mk;
    return pt(r);
}
static int lx_pthread_key_delete(unsigned k) { return pt(pthread_key_delete((pthread_key_t)k)); }
static void *lx_pthread_getspecific(unsigned k) { return pthread_getspecific((pthread_key_t)k); }
static int lx_pthread_setspecific(unsigned k, const void *v) { return pt(pthread_setspecific((pthread_key_t)k, v)); }

/* glibc pthread_attr_t is 56 bytes: holds a pointer to a macOS attr */
struct lx_attr { pthread_attr_t *a; char pad[48]; };
_Static_assert(sizeof(struct lx_attr) == 56, "glibc pthread_attr_t is 56 bytes");
static int lx_pthread_attr_init(struct lx_attr *x)
{
    x->a = malloc(sizeof *x->a);
    return x->a ? pt(pthread_attr_init(x->a)) : 12;
}
static int lx_pthread_attr_setstack(struct lx_attr *x, void *addr, size_t sz)
{
    if (!x->a) return 22;
    int r = pthread_attr_setstacksize(x->a, sz);
    if (!r) r = pthread_attr_setstackaddr(x->a, (char *)addr + sz);   /* macOS takes the top of the stack */
    return pt(r);
}
static int lx_pthread_create(pthread_t *t, const struct lx_attr *x, void *(*fn)(void *), void *arg)
{
    return pt(pthread_create(t, x ? x->a : NULL, fn, arg));
}
static int lx_pthread_detach(pthread_t t) { return pt(pthread_detach(t)); }
static int lx_pthread_join(pthread_t t, void **r) { return pt(pthread_join(t, r)); }

/* glibc pthread_spinlock_t is a 4-byte int */
static int lx_pthread_spin_init(_Atomic int *s, int pshared) { (void)pshared; atomic_store(s, 0); return 0; }
static int lx_pthread_spin_destroy(_Atomic int *s) { (void)s; return 0; }
static int lx_pthread_spin_lock(_Atomic int *s)
{
    int z = 0;
    while (!atomic_compare_exchange_weak(s, &z, 1)) { z = 0; sched_yield(); }
    return 0;
}
static int lx_pthread_spin_trylock(_Atomic int *s) { int z = 0; return atomic_compare_exchange_strong(s, &z, 1) ? 0 : 16; }
static int lx_pthread_spin_unlock(_Atomic int *s) { atomic_store(s, 0); return 0; }

/* glibc sem_t is 32 bytes; macOS has no unnamed POSIX semaphores, so use a dispatch semaphore */
struct lx_sem { dispatch_semaphore_t s; char pad[24]; };
_Static_assert(sizeof(struct lx_sem) == 32, "glibc sem_t is 32 bytes");
static int lx_sem_init(struct lx_sem *x, int pshared, unsigned v)
{
    if (pshared) TRACE("process-shared semaphore requested; process-private on macOS");
    x->s = dispatch_semaphore_create((long)v);
    return x->s ? 0 : fail_lx(12);
}
static int lx_sem_destroy(struct lx_sem *x) { if (x->s) dispatch_release(x->s); x->s = NULL; return 0; }
static int lx_sem_post(struct lx_sem *x) { dispatch_semaphore_signal(x->s); return 0; }
static int lx_sem_wait(struct lx_sem *x) { dispatch_semaphore_wait(x->s, DISPATCH_TIME_FOREVER); return 0; }
static int lx_sem_trywait(struct lx_sem *x) { return dispatch_semaphore_wait(x->s, DISPATCH_TIME_NOW) ? fail_lx(11) : 0; }
static int lx_sem_timedwait(struct lx_sem *x, const struct timespec *abs)
{
    dispatch_time_t t = dispatch_walltime(abs, 0);
    return dispatch_semaphore_wait(x->s, t) ? fail_lx(110) : 0;
}

/* --- dynamic loading ------------------------------------------------------------------------------------------------ */

enum { kMaxLibs = 16, kMaxAliases = 32 };
static struct elfobj *gLibs[kMaxLibs];
static int gLibN;
static struct { char *name; struct elfobj *o; } gAlias[kMaxAliases];
static int gAliasN;
static _Thread_local char tDlerr[512];
static _Thread_local int tDlerrSet;
static char gGlibcHandle;   /* handle for libc.so.6 & co: symbols come from this shim */

void shim_register_alias(const char *soname, struct elfobj *o)
{
    if (gAliasN < kMaxAliases) { gAlias[gAliasN].name = strdup(soname); gAlias[gAliasN].o = o; gAliasN++; }
}
struct elfobj *shim_load_library(const char *path, char *err, size_t errlen)
{
    if (gLibN >= kMaxLibs) { snprintf(err, errlen, "too many libraries"); return NULL; }
    struct elfobj *o = elf_load(path, shim_resolve, err, errlen);
    if (!o) return NULL;
    gLibs[gLibN++] = o;
    char **env = *_NSGetEnviron();
    char *argv[] = { "cuda-shim", NULL };
    elf_run_init(o, 1, argv, env);
    return o;
}
/* /proc/self/maps in Linux format, listing the ELF objects this shim loaded (libcuda locates itself through it). */
static char *proc_self_maps(void)
{
    size_t cap = 512 + (size_t)gLibN * 8 * (PATH_MAX + 128), len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = 0;
    /* One line per PT_LOAD segment, with the file's real device and inode, as Linux shows a mapped shared object. */
    for (int i = 0; i < gLibN; i++) {
        const unsigned long b = (unsigned long)elf_base(gLibs[i]);
        char real[PATH_MAX];
        const char *p = realpath(elf_path(gLibs[i]), real) ? real : elf_path(gLibs[i]);
        struct stat st;
        memset(&st, 0, sizeof st);
        stat(p, &st);
        uint64_t va, msz, off; uint32_t fl;
        for (int s = 0; elf_segment(gLibs[i], s, &va, &msz, &off, &fl); s++) {
            const unsigned long lo = (b + va) & ~0xFFFUL, hi = (b + va + msz + 0xFFF) & ~0xFFFUL;
            len += (size_t)snprintf(out + len, cap - len, "%lx-%lx %c%c%cp %08llx %02x:%02x %llu                          %s\n",
                                    lo, hi, (fl & 4) ? 'r' : '-', (fl & 2) ? 'w' : '-', (fl & 1) ? 'x' : '-',
                                    (unsigned long long)(off & ~0xFFFULL), major(st.st_dev) & 0xFF, minor(st.st_dev) & 0xFF,
                                    (unsigned long long)st.st_ino, p);
        }
    }
    int here;
    const unsigned long sp = (unsigned long)&here & ~0xFFFFUL;
    len += (size_t)snprintf(out + len, cap - len, "%lx-%lx rw-p 00000000 00:00 0                          [stack]\n",
                            sp - 0x800000UL, sp + 0x10000UL);
    return out;
}
static int is_glibc_name(const char *n)
{
    static const char *const libs[] = { "libc.so", "libpthread.so", "libdl.so", "librt.so", "libm.so", "ld-linux" };
    for (size_t i = 0; i < sizeof libs / sizeof libs[0]; i++) if (strstr(n, libs[i])) return 1;
    return 0;
}
static void *lx_dlopen(const char *name, int flags)
{
    (void)flags;
    if (!name) return &gGlibcHandle;                      /* dlopen(NULL): the main program */
    const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
    for (int i = 0; i < gAliasN; i++) if (!strcmp(gAlias[i].name, base)) { TRACE("dlopen %s -> loaded %s", name, elf_path(gAlias[i].o)); return gAlias[i].o; }
    if (is_glibc_name(base)) { TRACE("dlopen %s -> glibc shim", name); return &gGlibcHandle; }
    /* other NVIDIA libraries (e.g. libnvidia-ptxjitcompiler.so.1) are looked up in CUDASHIM_LIBDIR */
    const char *dir = getenv("CUDASHIM_LIBDIR");
    if (dir && !strncmp(base, "libnvidia-", 10)) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, base);
        char err[256];
        struct elfobj *o = shim_load_library(path, err, sizeof err);
        if (o) { shim_register_alias(base, o); TRACE("dlopen %s -> loaded from CUDASHIM_LIBDIR", name); return o; }
        TRACE("dlopen %s failed: %s", name, err);
    }
    snprintf(tDlerr, sizeof tDlerr, "%s: cannot open shared object file (cuda-shim)", name);
    tDlerrSet = 1;
    TRACE("dlopen %s -> NULL", name);
    return NULL;
}
static void *lx_dlmopen(long lmid, const char *name, int flags) { (void)lmid; return lx_dlopen(name, flags); }
static void *lx_dlsym(void *h, const char *sym)
{
    void *r = NULL;
    if (h == &gGlibcHandle) r = shim_resolve(sym, 1);
    else if (h == NULL || h == (void *)-1L) {              /* RTLD_DEFAULT / RTLD_NEXT */
        for (int i = 0; i < gLibN && !r; i++) r = elf_sym(gLibs[i], sym);
        if (!r) r = shim_resolve(sym, 1);
    } else r = elf_sym((struct elfobj *)h, sym);
    if (!r) { snprintf(tDlerr, sizeof tDlerr, "undefined symbol: %s", sym); tDlerrSet = 1; }
    TRACE("dlsym %s -> %p", sym, r);
    return r;
}
static void *lx_dlvsym(void *h, const char *sym, const char *ver) { (void)ver; return lx_dlsym(h, sym); }
static int lx_dlclose(void *h) { (void)h; return 0; }   /* libraries stay loaded */
static char *lx_dlerror(void) { if (!tDlerrSet) return NULL; tDlerrSet = 0; return tDlerr; }

/* Traced in CUDASHIM_TRACE mode: which variables the library reads, and the text it formats (its own log lines). */
static char *lx_getenv(const char *n)
{
    char *v = getenv(n);
    TRACE("getenv %s -> %s", n, v ? v : "(unset)");
    return v;
}
static int lx_vsnprintf(char *b, size_t n, const char *fmt, va_list ap)
{
    int r = vsnprintf(b, n, fmt, ap);
    if (shim_trace() && b && n) TRACE("formatted: \"%.*s\"", (int)(n - 1 < 300 ? n - 1 : 300), b);
    return r;
}
static int lx_snprintf(char *b, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = lx_vsnprintf(b, n, fmt, ap);
    va_end(ap);
    return r;
}

/* --- the table ------------------------------------------------------------------------------------------------------ */

static void *const gDataPlaceholder = NULL;
struct entry { const char *name; void *addr; };
#define P(n) { #n, (void *)&n }            /* pass-through to the macOS function of the same name */
#define W(n) { #n, (void *)&lx_##n }       /* shim wrapper */
#define A(n, f) { #n, (void *)&f }         /* alias */

static const struct entry gTable[] = {
    /* process, errno, assert */
    W(__errno_location), W(__assert_fail), W(__cxa_atexit), W(__cxa_finalize), W(__register_atfork),
    P(abort), P(exit), W(getenv), P(setenv), P(unsetenv), P(getpid), P(getuid), P(geteuid), P(getegid),
    P(gethostname), P(getpagesize), P(time), P(localtime_r), P(gettimeofday), P(nanosleep), P(usleep), P(sched_yield),
    A(_setjmp, _setjmp), A(longjmp, _longjmp),
    /* memory and strings */
    P(malloc), P(calloc), P(realloc), P(free), W(memalign), P(memchr), P(memcmp), P(memcpy), P(memmove), P(memset),
    P(strlen), P(strnlen), P(strcmp), P(strncmp), P(strcasecmp), P(strncasecmp), P(strcpy), P(strncpy), P(strcat),
    P(strncat), P(strchr), P(strrchr), P(strstr), P(strcasestr), P(strpbrk), P(strcspn), P(strdup), P(strtok),
    P(strtok_r), P(strcoll), P(strtod), P(strtol), P(strtoll), P(strtoul), P(strtoull), W(strerror), W(strerror_r),
    P(isalnum), P(isspace), P(tolower), P(toupper), P(qsort), P(bsearch), P(ceil), P(pow), P(log10), P(lround),
    /* stdio */
    P(printf), P(fprintf), P(vfprintf), W(snprintf), P(sprintf), W(vsnprintf), P(vsprintf), P(sscanf), P(fscanf),
    A(__isoc99_sscanf, sscanf), A(__isoc99_fscanf, fscanf), P(puts), P(putchar), P(fputc), P(fputs), A(_IO_putc, putc),
    P(fwrite), P(fread), P(fflush), P(fclose), P(feof), P(ferror), P(fgets), P(fileno), P(fseek), P(ftell),
    A(__getdelim, getdelim), W(fopen), A(fopen64, lx_fopen), W(fdopen), W(getopt_long),
    /* files */
    W(open), A(open64, lx_open), W(openat64), W(read), W(write), W(close), W(lseek), W(access), W(faccessat),
    W(readlink), W(realpath), W(mkdir), W(rmdir), W(unlink), W(remove), W(chmod), W(fchmod), W(chown), W(symlink),
    W(dup), W(dup2), W(dup3), W(fcntl), W(ftruncate), W(flock), W(mkfifo), W(pipe), W(pipe2), W(poll), W(ioctl),
    W(accept4), W(sched_getcpu), W(pthread_getaffinity_np), W(pthread_setaffinity_np),
    W(__xstat), A(__xstat64, lx___xstat), W(__fxstat), A(__fxstat64, lx___fxstat), W(__xmknod),
    W(opendir), W(readdir), W(closedir), W(scandir64), W(alphasort64), W(nftw), W(shm_open), W(shm_unlink),
    /* memory mapping */
    W(mmap), A(mmap64, lx_mmap), W(munmap), W(mprotect), W(madvise), W(msync), W(mremap), W(mlock), W(munlock),
    /* system information */
    W(sysconf), W(get_nprocs), W(sysinfo), W(uname), W(getrlimit), W(setrlimit), W(clock_gettime),
    W(sched_get_priority_max), W(sched_get_priority_min),
    /* signals, processes, IPC */
    W(sigaction), W(pthread_kill), W(fork), W(execvp), W(execle), W(posix_spawn), W(waitpid),
    A(posix_spawn_file_actions_init, lx_spawn_fa_noop), A(posix_spawn_file_actions_destroy, lx_spawn_fa_noop),
    A(posix_spawn_file_actions_adddup2, lx_spawn_fa_noop), A(posix_spawn_file_actions_addopen, lx_spawn_fa_noop),
    W(socket), A(bind, lx_enosys), A(connect, lx_enosys), A(listen, lx_enosys), A(getsockopt, lx_enosys),
    A(setsockopt, lx_enosys), A(recvmsg, lx_enosys), A(sendmsg, lx_enosys), A(shutdown, lx_enosys),
    W(eventfd), W(timerfd_create), A(timerfd_settime, lx_enosys), W(syscall),
    /* pthread, semaphores */
    W(pthread_create), W(pthread_detach), W(pthread_join), P(pthread_self), W(pthread_once),
    W(pthread_key_create), W(pthread_key_delete), W(pthread_getspecific), W(pthread_setspecific),
    W(pthread_attr_init), W(pthread_attr_setstack),
    W(pthread_mutexattr_init), W(pthread_mutexattr_destroy), W(pthread_mutexattr_settype),
    W(pthread_mutexattr_setpshared), W(pthread_mutexattr_setprotocol),
    W(pthread_mutex_init), W(pthread_mutex_destroy), W(pthread_mutex_lock), W(pthread_mutex_trylock),
    W(pthread_mutex_unlock),
    W(pthread_condattr_init), W(pthread_condattr_setpshared), W(pthread_cond_init), W(pthread_cond_destroy),
    W(pthread_cond_wait), W(pthread_cond_timedwait), W(pthread_cond_signal), W(pthread_cond_broadcast),
    W(pthread_rwlockattr_init), W(pthread_rwlockattr_destroy), W(pthread_rwlockattr_setpshared),
    W(pthread_rwlock_init), W(pthread_rwlock_destroy), W(pthread_rwlock_rdlock), W(pthread_rwlock_wrlock),
    W(pthread_rwlock_trywrlock), W(pthread_rwlock_unlock),
    W(pthread_spin_init), W(pthread_spin_destroy), W(pthread_spin_lock), W(pthread_spin_trylock),
    W(pthread_spin_unlock),
    W(sem_init), W(sem_destroy), W(sem_post), W(sem_wait), W(sem_trywait), W(sem_timedwait),
    /* dynamic loading */
    W(dlopen), W(dlmopen), W(dlsym), W(dlvsym), W(dlclose), W(dlerror),
    /* data objects */
    { "stdout", &lx_stdout_var }, { "stderr", &lx_stderr_var }, { "optarg", &lx_optarg_var }, { "optind", &lx_optind_var },
};

/* CUDASHIM_TRACE=2: log every call from the loaded library into the shim (ltrace-style, interface only). */
static void call_log(const char *name) { fprintf(stderr, "cuda-shim: call %s\n", name); }
static int call_traced(const char *name)
{
    static const char *const quiet[] = { "memcpy", "memset", "memmove", "memcmp", "memchr", "strlen", "strnlen",
        "strcmp", "strncmp", "strchr", "strrchr", "strcpy", "strncpy", "strstr", "malloc", "calloc", "realloc", "free",
        "__errno_location", "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_getspecific", "pthread_setspecific",
        "_setjmp", "longjmp", "pthread_rwlock_rdlock", "pthread_rwlock_unlock", "isspace", "isalnum", "tolower",
        "toupper", "stdout", "stderr", "optarg", "optind" };
    const char *e = getenv("CUDASHIM_TRACE");
    if (!e || strcmp(e, "2") != 0) return 0;
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; i++) if (!strcmp(quiet[i], name)) return 0;
    return 1;
}

void *shim_resolve(const char *name, int weak)
{
    for (size_t i = 0; i < sizeof gTable / sizeof gTable[0]; i++)
        if (!strcmp(gTable[i].name, name))
            return call_traced(name) ? elf_make_trace_stub(name, gTable[i].addr, call_log) : gTable[i].addr;
    if (!weak) TRACE("no shim for %s (trap stub)", name);
    (void)gDataPlaceholder;
    return NULL;
}

void shim_init(void)
{
    lx_stdout_var = stdout;
    lx_stderr_var = stderr;
    atexit(run_all_atexit);
}

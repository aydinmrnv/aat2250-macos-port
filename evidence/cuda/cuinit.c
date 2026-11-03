/* Reference run on Linux: load libcuda and call cuInit(0), same as cuda-probe does on macOS. */
#include <dlfcn.h>
#include <stdio.h>

int main(void)
{
    void *h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    int (*init)(unsigned) = (int (*)(unsigned))dlsym(h, "cuInit");
    int (*name)(int, const char **) = (int (*)(int, const char **))dlsym(h, "cuGetErrorName");
    fprintf(stderr, "=== calling cuInit ===\n");
    int r = init(0);
    const char *s = "?";
    if (name) name(r, &s);
    fprintf(stderr, "=== cuInit returned %d %s ===\n", r, s);
    printf("cuInit -> %d %s\n", r, s);
    return 0;
}

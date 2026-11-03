/* Prints the page size an x86_64 process sees (4096 on Intel Macs; checks what Rosetta reports). */
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("getpagesize %d, sysconf %ld, 4 KB anonymous mmap at %p\n", getpagesize(), sysconf(_SC_PAGESIZE), p);
    return 0;
}

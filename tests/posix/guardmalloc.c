/* Electric-Fence style allocator for LD_PRELOAD, for machines without an
 * AddressSanitizer runtime. Every block ends exactly at a PROT_NONE guard page
 * (reads/writes past the end fault) and freed blocks become PROT_NONE and are
 * never reused (use-after-free faults). Alignment padding means overruns of
 * less than 16 bytes past an odd-sized block go unnoticed.
 *
 *   cc -shared -fPIC -O1 guardmalloc.c -o guardmalloc.so
 *   LD_PRELOAD=./guardmalloc.so ./gif_dump ...
 *
 * Memory hungry by design: use for the decoder tests, not for long runs. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALIGN 16

typedef struct Header {
    size_t size;      /* Requested size. */
    size_t map_len;   /* Whole mapping, guard page included. */
    void *map;
} Header;

static size_t page(void)
{
    static size_t p;
    if (!p) p = (size_t)sysconf(_SC_PAGESIZE);
    return p;
}

static void *guard_alloc(size_t size, size_t align)
{
    if (align < ALIGN) align = ALIGN;
    size_t ps = page();
    size_t body = (size + align - 1) & ~(align - 1);
    size_t need = body + sizeof(Header) + align;
    size_t data_len = (need + ps - 1) & ~(ps - 1);
    size_t map_len = data_len + ps;
    char *map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) { errno = ENOMEM; return NULL; }
    mprotect(map + data_len, ps, PROT_NONE);
    char *user = map + data_len - body;
    user = (char *)((uintptr_t)user & ~(uintptr_t)(align - 1));
    Header *h = (Header *)(user - sizeof(Header));
    h->size = size;
    h->map_len = map_len;
    h->map = map;
    return user;
}

static Header *header(void *p)
{
    return (Header *)((char *)p - sizeof(Header));
}

void *malloc(size_t size) { return guard_alloc(size, ALIGN); }

void free(void *p)
{
    if (!p) return;
    Header *h = header(p);
    /* Keep the address range reserved but inaccessible: any later access faults. */
    mprotect(h->map, h->map_len, PROT_NONE);
}

void *calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) { errno = ENOMEM; return NULL; }
    return guard_alloc(n * size, ALIGN); /* Fresh anonymous pages are zeroed. */
}

void *realloc(void *p, size_t size)
{
    if (!p) return malloc(size);
    if (!size) { free(p); return NULL; }
    void *q = malloc(size);
    if (!q) return NULL;
    size_t old = header(p)->size;
    memcpy(q, p, old < size ? old : size);
    free(p);
    return q;
}

int posix_memalign(void **out, size_t align, size_t size)
{
    void *p = guard_alloc(size, align);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

void *aligned_alloc(size_t align, size_t size) { return guard_alloc(size, align); }
void *memalign(size_t align, size_t size) { return guard_alloc(size, align); }
size_t malloc_usable_size(void *p) { return p ? header(p)->size : 0; }

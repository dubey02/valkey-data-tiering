/* jemalloc-to-libc shims for sanitizer builds.
 *
 * libflashcache.a is always compiled against jemalloc (deps/flashcache
 * hardcodes <jemalloc/jemalloc.h>), but SANITIZER builds switch Valkey to
 * MALLOC=libc so ASan can interpose the allocator. Linking the real
 * jemalloc into an ASan binary would hide FlashCache's heap from the
 * sanitizer; instead, forward the handful of je_* symbols FlashCache
 * uses to libc, keeping every allocation ASan-visible.
 *
 * Only linked when SANITIZER is set (see Makefile). */
#include <stdlib.h>
#include <malloc.h>

void *je_malloc(size_t size) {
    return malloc(size);
}

void *je_calloc(size_t nmemb, size_t size) {
    return calloc(nmemb, size);
}

void *je_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

void je_free(void *ptr) {
    free(ptr);
}

size_t je_malloc_usable_size(void *ptr) {
    return malloc_usable_size(ptr);
}

int je_posix_memalign(void **memptr, size_t alignment, size_t size) {
    return posix_memalign(memptr, alignment, size);
}

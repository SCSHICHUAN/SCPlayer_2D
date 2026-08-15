/* Thin malloc wrappers so vendored plc.c links without full SpanDSP alloc.c */
#include "config.h"
#include <stdlib.h>
#include "spandsp/telephony.h"
#include "spandsp/alloc.h"

SPAN_DECLARE(void *) span_alloc(size_t size)
{
    return malloc(size);
}

SPAN_DECLARE(void *) span_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}

SPAN_DECLARE(void) span_free(void *ptr)
{
    free(ptr);
}

SPAN_DECLARE(void *) span_aligned_alloc(size_t alignment, size_t size)
{
    (void)alignment;
    return malloc(size);
}

SPAN_DECLARE(void) span_aligned_free(void *ptr)
{
    free(ptr);
}

SPAN_DECLARE(int) span_mem_allocators(span_alloc_t custom_alloc,
                                      span_realloc_t custom_realloc,
                                      span_free_t custom_free,
                                      span_aligned_alloc_t custom_aligned_alloc,
                                      span_aligned_free_t custom_aligned_free)
{
    (void)custom_alloc;
    (void)custom_realloc;
    (void)custom_free;
    (void)custom_aligned_alloc;
    (void)custom_aligned_free;
    return 0;
}

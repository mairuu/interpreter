#include "memory.h"

#include <stdlib.h>
#include <string.h>

void *_allocate_zero(Allocator *al, size_t size) {
  void *ptr = al_alloc(al, size);
  if (ptr) {
    memset(ptr, 0, size);
  }
  return ptr;
}

static void *_heap_alloc(void *ctx, size_t size) {
  (void)ctx; // unused
  return malloc(size);
}

static void *_heap_realloc(void *ctx, void *ptr, size_t old_size,
                           size_t new_size) {
  (void)ctx;      // unused
  (void)old_size; // unused
  return realloc(ptr, new_size);
}

static void _heap_free(void *ctx, void *ptr, size_t size) {
  (void)ctx;  // unused
  (void)size; // unused
  free(ptr);
}

Allocator heap_allocator_create(void) {
  return (Allocator){
      .alloc = _heap_alloc,
      .realloc = _heap_realloc,
      .free = _heap_free,
      .ctx = NULL,
  };
}
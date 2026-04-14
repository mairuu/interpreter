#include "dynamic_array.h"

#include "memory.h"
#include <string.h>

void *_array_new(Allocator *al, size_t element_size) {
  size_t size = sizeof(ArrayHeader) + element_size * ARRAY_INITIAL_CAPACITY;
  ArrayHeader *header = al_alloc(al, size);
  header->count = 0;
  header->capacity = ARRAY_INITIAL_CAPACITY;
  return (void *)(header + 1);
}

void _array_init(void **array, size_t element_size, Allocator *al) {
  *array = _array_new(al, element_size);
}

void _array_free(void **array, size_t element_size, Allocator *al) {
  _array_destory(*array, element_size, al);
  *array = NULL;
}

void _array_destory(void *array, size_t element_size, Allocator *al) {
  if (!array) {
    return;
  }

  ArrayHeader *header = (ArrayHeader *)array - 1;
  al_free(al, header, sizeof(ArrayHeader) + header->capacity * element_size);
}

bool _array_resize(void **array, size_t element_size, size_t new_capacity,
                   Allocator *al) {
  size_t new_size = sizeof(ArrayHeader) + new_capacity * element_size;

  if (!*array) {
    ArrayHeader *header = al_alloc(al, new_size);
    if (!header) {
      return false;
    }
    header->count = 0;
    header->capacity = new_capacity;
    *array = (void *)(header + 1);
    return true;
  }

  ArrayHeader *header = (ArrayHeader *)(*array) - 1;
  size_t old_size = sizeof(ArrayHeader) + header->capacity * element_size;
  void *new_block = al_realloc(al, header, old_size, new_size);
  if (!new_block) {
    return false;
  }
  header = (ArrayHeader *)new_block;
  header->capacity = new_capacity;
  *array = (void *)(header + 1);
  return true;
}

bool _array_reserve(void **array, size_t element_size, size_t n,
                    Allocator *al) {
  if (array_capacity(*array) >= n) {
    return true;
  }
  size_t new_capacity = array_capacity(*array) == 0 ? ARRAY_INITIAL_CAPACITY
                                                    : array_capacity(*array);
  while (new_capacity < n) {
    new_capacity *= ARRAY_GROWTH_FACTOR;
  }
  return _array_resize(array, element_size, new_capacity, al);
}

void *_array_copy(void *array, size_t element_size, Allocator *al) {
  if (!array) {
    return NULL;
  }
  ArrayHeader *header = (ArrayHeader *)array - 1;
  size_t size = sizeof(ArrayHeader) + header->capacity * element_size;
  void *new_block = al_alloc(al, size);
  if (!new_block) {
    return NULL;
  }
  memcpy(new_block, header, size);
  return (void *)((ArrayHeader *)new_block + 1);
}
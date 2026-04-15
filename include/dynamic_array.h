#pragma once

#include "memory.h"
#include <assert.h>
#include <stddef.h>

#define ARRAY_INITIAL_CAPACITY 4
#define ARRAY_GROWTH_FACTOR 2

typedef struct {
  size_t count;
  size_t capacity;
} ArrayHeader;

#define array_new(al, type) (type *)_array_new(al, sizeof(type))

#define array_init(array, type, al)                                            \
  _array_init((void **)&(array), sizeof(type), al)

#define array_destory(array, al) _array_destory(array, al, sizeof(*(array)))

#define array_free(array, al)                                                  \
  _array_free((void **)&(array), sizeof(*(array)), al)

#define array_count(array) ((array) ? ((ArrayHeader *)(array) - 1)->count : 0)

#define array_capacity(array)                                                  \
  ((array) ? ((ArrayHeader *)(array) - 1)->capacity : 0)

#define array_push(array, element, al)                                         \
  do {                                                                         \
    assert(array != NULL);                                                     \
    ArrayHeader *header = (ArrayHeader *)array - 1;                            \
    if (header->count >= header->capacity) {                                   \
      size_t new_capacity = header->capacity * ARRAY_GROWTH_FACTOR;            \
      if (!_array_resize((void **)&array, sizeof(*array), new_capacity, al)) { \
        break;                                                                 \
      }                                                                        \
      header = (ArrayHeader *)array - 1;                                       \
    }                                                                          \
    array[header->count++] = element;                                          \
  } while (0)

// caller must ensure array is not empty before calling this macro
#define array_pop(array) ((array)[--((ArrayHeader *)(array) - 1)->count])

#define array_peek(array) ((array)[((ArrayHeader *)(array) - 1)->count - 1])

// reserve at least n capacity for the array
#define array_reserve(array, n, al)                                            \
  _array_reserve((void **)&(array), sizeof(*(array)), n, al)

#define array_copy(array, al)                                                  \
  (typeof(array))_array_copy(array, sizeof(*(array)), al)

void *_array_new(Allocator *al, size_t element_size);
void _array_init(void **array, size_t element_size, Allocator *al);

void _array_destory(void *array, size_t element_size, Allocator *al);
void _array_free(void **array, size_t element_size, Allocator *al);

bool _array_resize(void **array, size_t element_size, size_t new_capacity,
                   Allocator *al);

bool _array_reserve(void **array, size_t element_size, size_t new_capacity,
                    Allocator *al);

void *_array_copy(void *array, size_t element_size, Allocator *al);
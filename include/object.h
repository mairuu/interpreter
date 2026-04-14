#pragma once

#include "memory.h"
#include "value.h"

typedef enum {
  OBJECT_STRING,
} ObjectType;

struct Object {
  Object *next;
  ObjectType type;
};

typedef struct {
  Object object;
  int length;
  char *chars;
  uint32_t hash;
  bool is_interned;
} ObjectString;

void object_print(Object *obj);

static inline bool value_is_object_type(Value value, ObjectType type) {
  return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

#define IS_STRING(value) value_is_object_type(value, OBJECT_STRING)

#define AS_STRING(value) ((ObjectString *)AS_OBJECT(value))

// dispatch to the appropriate free function based on the object type
void object_free(Object **obj, Allocator *al);

// for temporary strings, used for hashing and lookup, not heap allocated
ObjectString object_string_create(const char *chars, int length, uint32_t hash);
// takes ownership of chars
ObjectString *object_string_new(Allocator *al, char *chars, int length,
                                uint32_t hash);
void object_string_free(ObjectString **obj, Allocator *al);
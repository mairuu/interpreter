#include "object.h"
#include "value.h"

#include <assert.h>
#include <stdio.h>

void object_print(Object *obj) {
  switch (obj->type) {
  case OBJECT_STRING:
    printf("%s", ((ObjectString *)obj)->chars);
    break;
  }
}

static void *object_new(Allocator *al, size_t size, ObjectType type) {
  Object *obj = al_alloc(al, size);
  obj->type = type;
  return obj;
}

#define OBJECT_NEW(al, type, object_type)                                      \
  (type *)object_new(al, sizeof(type), object_type)

ObjectString object_string_create(const char *chars, int length,
                                  uint32_t hash) {
  return (ObjectString){
      .object = {.type = OBJECT_STRING},
      .is_interned = false,
      .chars = (char *)chars,
      .length = length,
      .hash = hash,
  };
}

ObjectString *object_string_new(Allocator *al, char *chars, int length,
                                uint32_t hash) {
  ObjectString *object = OBJECT_NEW(al, ObjectString, OBJECT_STRING);
  object->is_interned = false;
  object->chars = chars;
  object->length = length;
  object->hash = hash;
  return object;
}

void object_string_free(ObjectString **obj, Allocator *al) {
  al_free(al, (*obj)->chars, (*obj)->length + 1);
  al_free(al, *obj, sizeof(ObjectString));
  *obj = NULL;
}

void object_free(Object **obj, Allocator *al) {
  switch ((*obj)->type) {
  case OBJECT_STRING:
    object_string_free((ObjectString **)obj, al);
    break;
  default:
    assert(false && "unknown object type");
  }
}
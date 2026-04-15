#include "object.h"
#include "value.h"

#include <assert.h>
#include <chunk.h>
#include <stdio.h>

void object_print(Object *obj) {
  switch (obj->type) {
  case OBJECT_STRING:
    printf("%s", ((ObjectString *)obj)->chars);
    break;
  case OBJECT_FUNCTION:
    if (((ObjectFunction *)obj)->name) {
      printf("<fn %s>", ((ObjectFunction *)obj)->name->chars);
    } else {
      printf("<script>");
    }
    break;
  case OBJECT_NATIVE:
    printf("<native fn>");
    break;
  }
}

static void *object_new(Allocator *al, size_t size, ObjectType type) {
  Object *obj = al_alloc(al, size);
  obj->type = type;
  obj->next = NULL;
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

ObjectFunction *object_function_new(Allocator *al) {
  ObjectFunction *object = OBJECT_NEW(al, ObjectFunction, OBJECT_FUNCTION);
  object->arity = 0;
  object->name = NULL;
  chunk_init(&object->chunk);
  return object;
}

void object_function_free(ObjectFunction **obj, Allocator *al) {
  if (*obj) {
    chunk_destroy(&(*obj)->chunk, al);
    al_free(al, *obj, sizeof(ObjectFunction));
    *obj = NULL;
  }
}

ObjectNative *object_native_new(Allocator *al, NavtiveFunc function) {
  ObjectNative *object = OBJECT_NEW(al, ObjectNative, OBJECT_NATIVE);
  object->function = function;
  return object;
}
void object_native_free(ObjectNative **obj, Allocator *al) {
  if (*obj) {
    al_free(al, *obj, sizeof(ObjectNative));
    *obj = NULL;
  }
}

void object_free(Object **obj, Allocator *al) {
  switch ((*obj)->type) {
  case OBJECT_STRING:
    object_string_free((ObjectString **)obj, al);
    break;
  case OBJECT_FUNCTION:
    object_function_free((ObjectFunction **)obj, al);
    break;
  case OBJECT_NATIVE:
    object_native_free((ObjectNative **)obj, al);
    break;
  default:
    assert(false && "unknown object type");
  }
}

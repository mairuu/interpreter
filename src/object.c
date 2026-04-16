#include "object.h"
#include "value.h"

#include <assert.h>
#include <chunk.h>
#include <stdio.h>

static void object_function_print(ObjectFunction *function) {
  printf("<fn %s>", function->name ? function->name->chars : "_");
}

void object_print(Object *obj) {
  switch (obj->type) {
  case OBJECT_STRING:
    printf("%s", ((ObjectString *)obj)->chars);
    break;
  case OBJECT_FUNCTION:
    object_function_print((ObjectFunction *)obj);
    break;
  case OBJECT_UPVALUE:
    printf("<upvalue>");
    break;
  case OBJECT_CLOSURE:
    object_function_print(((ObjectClosure *)obj)->function);
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
  ObjectString *str = OBJECT_NEW(al, ObjectString, OBJECT_STRING);
  str->is_interned = false;
  str->chars = chars;
  str->length = length;
  str->hash = hash;
  return str;
}

void object_string_free(ObjectString **obj, Allocator *al) {
  al_free(al, (*obj)->chars, (*obj)->length + 1);
  al_free(al, *obj, sizeof(ObjectString));
  *obj = NULL;
}

ObjectFunction *object_function_new(Allocator *al) {
  ObjectFunction *func = OBJECT_NEW(al, ObjectFunction, OBJECT_FUNCTION);
  func->arity = 0;
  func->upvalue_count = 0;
  func->name = NULL;
  chunk_init(&func->chunk);
  return func;
}

void object_function_free(ObjectFunction **obj, Allocator *al) {
  if (*obj) {
    chunk_destroy(&(*obj)->chunk, al);
    al_free(al, *obj, sizeof(ObjectFunction));
    *obj = NULL;
  }
}

ObjectUpvalue *object_upvalue_new(Allocator *al, Value *location) {
  ObjectUpvalue *upvalue = OBJECT_NEW(al, ObjectUpvalue, OBJECT_UPVALUE);
  upvalue->next = NULL;
  upvalue->location = location;
  upvalue->closed = NIL_VALUE;
  return upvalue;
}

void object_upvalue_free(ObjectUpvalue **obj, Allocator *al) {
  if (*obj) {
    al_free(al, *obj, sizeof(ObjectUpvalue));
    *obj = NULL;
  }
}

static inline size_t closure_size(int upvalue_count) {
  return sizeof(ObjectClosure) + sizeof(ObjectUpvalue *) * upvalue_count;
}

ObjectClosure *object_closure_new(Allocator *al, ObjectFunction *function) {
  ObjectClosure *closure = (ObjectClosure *)object_new(
      al, closure_size(function->upvalue_count), OBJECT_CLOSURE);
  closure->function = function;
  closure->upvalue_count = function->upvalue_count;
  for (int i = 0; i < closure->upvalue_count; i++) {
    closure->upvalues[i] = NULL;
  }
  return closure;
}

void object_closure_free(ObjectClosure **obj, Allocator *al) {
  if (*obj) {
    al_free(al, *obj, closure_size((*obj)->upvalue_count));
    *obj = NULL;
  }
}

ObjectNative *object_native_new(Allocator *al, NavtiveFunc function) {
  ObjectNative *native = OBJECT_NEW(al, ObjectNative, OBJECT_NATIVE);
  native->function = function;
  return native;
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
  case OBJECT_UPVALUE:
    object_upvalue_free((ObjectUpvalue **)obj, al);
    break;
  case OBJECT_CLOSURE:
    object_closure_free((ObjectClosure **)obj, al);
    break;
  case OBJECT_NATIVE:
    object_native_free((ObjectNative **)obj, al);
    break;
  default:
    assert(false && "unknown object type");
  }
}

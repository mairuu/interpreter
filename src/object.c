#include "object.h"
#include "dynamic_array.h"
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
  case OBJECT_STRUCT_DEFINITION:
    printf("<struct %s>", ((ObjectStructDefinition *)obj)->name->chars);
    break;
  case OBJECT_STRUCT_INSTANCE:
    printf("<instance of %s>", ((ObjectStructInstance *)obj)->def->name->chars);
    break;
  case OBJECT_TRAIT_DEFINITION:
    printf("<trait %s>", ((ObjectTraitDefinition *)obj)->name->chars);
    break;
  default:
    assert(false && "unknown object type");
  }
}

static void *object_new(Allocator *al, size_t size, ObjectType type) {
  Object *obj = al_alloc(al, size);
  obj->type = type;
  obj->next = NULL;
  obj->is_marked = false;
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
  if (!*obj) {
    return;
  }

  al_free(al, *obj, sizeof(ObjectUpvalue));
  *obj = NULL;
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
  if (!*obj) {
    return;
  }
  al_free(al, *obj, closure_size((*obj)->upvalue_count));
  *obj = NULL;
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

ObjectStructDefinition *object_struct_definition_new(Allocator *al,
                                                     ObjectString *name,
                                                     uint32_t definition_id) {
  ObjectStructDefinition *def =
      OBJECT_NEW(al, ObjectStructDefinition, OBJECT_STRUCT_DEFINITION);
  def->name = name;
  def->definition_id = definition_id;
  ht_init(&def->fields, al);
  return def;
}

void object_struct_definition_free(ObjectStructDefinition **obj,
                                   Allocator *al) {
  if (!*obj) {
    return;
  }

  ht_destroy(&(*obj)->fields, al);
  al_free(al, *obj, sizeof(ObjectStructDefinition));
  *obj = NULL;
}

static inline size_t struct_instance_size(ObjectStructDefinition *def) {
  return sizeof(ObjectStructInstance) + sizeof(Value) * def->fields.count;
}

ObjectStructInstance *object_struct_instance_new(Allocator *al,
                                                 ObjectStructDefinition *def) {
  ObjectStructInstance *instance = (ObjectStructInstance *)object_new(
      al, struct_instance_size(def), OBJECT_STRUCT_INSTANCE);
  instance->def = def;
  for (int i = 0; i < def->fields.count; i++) {
    instance->fields[i] = NIL_VALUE;
  }
  return instance;
}

void object_struct_instance_free(ObjectStructInstance **obj, Allocator *al) {
  if (!*obj) {
    return;
  }

  al_free(al, *obj, struct_instance_size((*obj)->def));
  *obj = NULL;
}

ObjectTraitDefinition *object_trait_definition_new(Allocator *al,
                                                   ObjectString *name,
                                                   uint32_t trait_id) {
  ObjectTraitDefinition *trait =
      OBJECT_NEW(al, ObjectTraitDefinition, OBJECT_TRAIT_DEFINITION);
  trait->name = name;
  trait->trait_id = trait_id;
  array_init(trait->method_names, ObjectString *, al);
  return trait;
}

void object_trait_definition_free(ObjectTraitDefinition **obj, Allocator *al) {
  if (!*obj) {
    return;
  }

  array_free((*obj)->method_names, al);
  al_free(al, *obj, sizeof(ObjectTraitDefinition));
  *obj = NULL;
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
  case OBJECT_STRUCT_DEFINITION:
    object_struct_definition_free((ObjectStructDefinition **)obj, al);
    break;
  case OBJECT_STRUCT_INSTANCE:
    object_struct_instance_free((ObjectStructInstance **)obj, al);
    break;
  case OBJECT_TRAIT_DEFINITION:
    object_trait_definition_free((ObjectTraitDefinition **)obj, al);
    break;
  default:
    assert(false && "unknown object type");
  }
}

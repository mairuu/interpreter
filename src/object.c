#include "object.h"
#include "dynamic_array.h"
#include "memory.h"
#include "value.h"

#include <assert.h>
#include <chunk.h>
#include <stdio.h>
#include <string.h>

int obj_print(char *buf, size_t size, Object *obj) {
  switch (obj->type) {
  case OBJECT_STRING:
    return snprintf(buf, size, "%s", ((ObjectString *)obj)->chars);
  case OBJECT_FUNCTION:
    return snprintf(buf, size, "<fn %s>",
                    ((ObjectFunction *)obj)->name
                        ? ((ObjectFunction *)obj)->name->chars
                        : "_");
    break;
  case OBJECT_UPVALUE:
    return snprintf(buf, size, "<upvalue>");
    break;
  case OBJECT_CLOSURE:
    return snprintf(buf, size, "<fn %s>",
                    ((ObjectClosure *)obj)->function->name
                        ? ((ObjectClosure *)obj)->function->name->chars
                        : "_");
    break;
  case OBJECT_NATIVE:
    return snprintf(buf, size, "<native fn>");
    break;
  case OBJECT_STRUCT_DEFINITION:
    return snprintf(buf, size, "<struct %s>",
                    ((ObjectStructDefinition *)obj)->name->chars);
    break;
  case OBJECT_STRUCT_INSTANCE:
    return snprintf(buf, size, "<instance of %s>",
                    ((ObjectStructInstance *)obj)->def->name->chars);
    break;
  case OBJECT_TRAIT_DEFINITION:
    return snprintf(buf, size, "<trait %s>",
                    ((ObjectTraitDefinition *)obj)->name->chars);
    break;
  case OBJECT_IMPL:
    return snprintf(buf, size, "<impl of %s for %s>",
                    ((ObjectImpl *)obj)->trait->name->chars,
                    ((ObjectImpl *)obj)->struct_def->name->chars);
    break;
  case OBJECT_TRAIT_OBJECT:
    return snprintf(buf, size, "<trait object of %s for %s>",
                    ((ObjectTraitObject *)obj)->impl->trait->name->chars,
                    ((ObjectTraitObject *)obj)->impl->struct_def->name->chars);
    break;
  case OBJECT_BOUND_METHOD:
    return snprintf(buf, size, "<fn %s>",
                    ((ObjectBoundMethod *)obj)->method->function->name->chars);
    break;
  default:
    assert(false && "unknown object type");
  }
  return -1;
}

static void *obj_new(Allocator *al, size_t size, ObjectType type) {
  Object *obj = al_alloc(al, size);
  obj->type = type;
  obj->next = NULL;
  obj->is_marked = false;
  return obj;
}

#define OBJECT_NEW(al, type, obj_type)                                         \
  (type *)obj_new(al, sizeof(type), obj_type)

ObjectString obj_string_create(const char *chars, int length, uint32_t hash) {
  return (ObjectString){
      .object = {.type = OBJECT_STRING},
      .is_interned = false,
      .chars = (char *)chars,
      .length = length,
      .hash = hash,
  };
}

ObjectString *obj_string_new(Allocator *al, char *chars, int length,
                             uint32_t hash) {
  ObjectString *str = OBJECT_NEW(al, ObjectString, OBJECT_STRING);
  str->is_interned = false;
  str->chars = chars;
  str->length = length;
  str->hash = hash;
  return str;
}

void obj_string_free(ObjectString **obj, Allocator *al) {
  al_free(al, (*obj)->chars, (*obj)->length + 1);
  al_free(al, *obj, sizeof(ObjectString));
  *obj = NULL;
}

bool obj_string_equals(ObjectString *a, ObjectString *b) {
  if (a->is_interned && b->is_interned) {
    return a == b;
  }
  if (a->length != b->length)
    return false;
  return memcmp(a->chars, b->chars, a->length) == 0;
}

static inline size_t function_size(int arity) {
  return sizeof(ObjectFunction) + sizeof(ObjectTraitDefinition *) * arity;
}

ObjectFunction *obj_function_new(Allocator *al, int arity) {
  ObjectFunction *func =
      (ObjectFunction *)obj_new(al, function_size(arity), OBJECT_FUNCTION);
  func->arity = arity;
  func->upvalue_count = 0;
  func->name = NULL;
  for (int i = 0; i < arity; i++) {
    func->constraints[i] = NULL;
  }
  chunk_init(&func->chunk);
  return func;
}

void obj_function_free(ObjectFunction **obj, Allocator *al) {
  if (!*obj) {
    return;
  }

  chunk_destroy(&(*obj)->chunk, al);
  al_free(al, *obj, function_size((*obj)->arity));
  *obj = NULL;
}

bool obj_function_bind_constraint(ObjectFunction *function, int param_idx,
                                  ObjectTraitDefinition *trait) {
  if (param_idx < 0 || param_idx >= function->arity) {
    return false; // param_idx out of bounds
  }
  function->constraints[param_idx] = trait;
  return true;
}

ObjectUpvalue *obj_upvalue_new(Allocator *al, Value *location) {
  ObjectUpvalue *upvalue = OBJECT_NEW(al, ObjectUpvalue, OBJECT_UPVALUE);
  upvalue->next = NULL;
  upvalue->location = location;
  upvalue->closed = NIL_VALUE;
  return upvalue;
}

void obj_upvalue_free(ObjectUpvalue **obj, Allocator *al) {
  if (!*obj) {
    return;
  }

  al_free(al, *obj, sizeof(ObjectUpvalue));
  *obj = NULL;
}

static inline size_t closure_size(int upvalue_count) {
  return sizeof(ObjectClosure) + sizeof(ObjectUpvalue *) * upvalue_count;
}

ObjectClosure *obj_closure_new(Allocator *al, ObjectFunction *function) {
  ObjectClosure *closure = (ObjectClosure *)obj_new(
      al, closure_size(function->upvalue_count), OBJECT_CLOSURE);
  closure->function = function;
  closure->upvalue_count = function->upvalue_count;
  for (int i = 0; i < closure->upvalue_count; i++) {
    closure->upvalues[i] = NULL;
  }
  return closure;
}

void obj_closure_free(ObjectClosure **obj, Allocator *al) {
  if (!*obj) {
    return;
  }
  al_free(al, *obj, closure_size((*obj)->upvalue_count));
  *obj = NULL;
}

ObjectNative *obj_native_new(Allocator *al, NativeFunc function) {
  ObjectNative *native = OBJECT_NEW(al, ObjectNative, OBJECT_NATIVE);
  native->function = function;
  return native;
}

void obj_native_free(ObjectNative **obj, Allocator *al) {
  if (*obj) {
    al_free(al, *obj, sizeof(ObjectNative));
    *obj = NULL;
  }
}

ObjectStructDefinition *obj_struct_definition_new(Allocator *al,
                                                  ObjectString *name,
                                                  uint32_t definition_id) {
  ObjectStructDefinition *def =
      OBJECT_NEW(al, ObjectStructDefinition, OBJECT_STRUCT_DEFINITION);
  def->name = name;
  def->definition_id = definition_id;
  array_init(def->impls, ObjectImpl *, al);
  ht_init(&def->fields, al);
  return def;
}

void obj_struct_definition_free(ObjectStructDefinition **obj, Allocator *al) {
  if (!*obj) {
    return;
  }

  ht_destroy(&(*obj)->fields, al);
  array_free((*obj)->impls, al);
  al_free(al, *obj, sizeof(ObjectStructDefinition));
  *obj = NULL;
}

ObjectImpl *obj_struct_definition_find_impl(ObjectStructDefinition *def,
                                            ObjectTraitDefinition *trait) {
  int impl_count = array_count(def->impls);
  for (int i = 0; i < impl_count; i++) {
    ObjectImpl *candidate = def->impls[i];
    if (candidate->trait->trait_id == trait->trait_id) {
      return candidate;
    }
  }
  return NULL;
}

static inline size_t struct_instance_size(ObjectStructDefinition *def) {
  return sizeof(ObjectStructInstance) + sizeof(Value) * def->fields.count;
}

ObjectStructInstance *obj_struct_instance_new(Allocator *al,
                                              ObjectStructDefinition *def) {
  ObjectStructInstance *instance = (ObjectStructInstance *)obj_new(
      al, struct_instance_size(def), OBJECT_STRUCT_INSTANCE);
  instance->def = def;
  for (int i = 0; i < def->fields.count; i++) {
    instance->fields[i] = NIL_VALUE;
  }
  return instance;
}

void obj_struct_instance_free(ObjectStructInstance **obj, Allocator *al) {
  if (!*obj) {
    return;
  }

  al_free(al, *obj, struct_instance_size((*obj)->def));
  *obj = NULL;
}

ObjectTraitDefinition *
obj_trait_definition_new(Allocator *al, ObjectString *name, uint32_t trait_id) {
  ObjectTraitDefinition *trait =
      OBJECT_NEW(al, ObjectTraitDefinition, OBJECT_TRAIT_DEFINITION);
  trait->name = name;
  trait->trait_id = trait_id;
  array_init(trait->method_names, ObjectString *, al);
  return trait;
}

void obj_trait_definition_free(ObjectTraitDefinition **obj, Allocator *al) {
  if (!*obj) {
    return;
  }

  array_free((*obj)->method_names, al);
  al_free(al, *obj, sizeof(ObjectTraitDefinition));
  *obj = NULL;
}

int obj_trait_find_slot(ObjectTraitDefinition *trait,
                        ObjectString *method_name) {
  int method_count = array_count(trait->method_names);
  for (int i = 0; i < method_count; i++) {
    if (obj_string_equals(trait->method_names[i], method_name)) {
      return i;
    }
  }
  return -1;
}

static inline size_t impl_size(ObjectTraitDefinition *trait) {
  return sizeof(ObjectImpl) +
         sizeof(Object *) * array_count(trait->method_names);
}

ObjectImpl *obj_impl_new(Allocator *al, ObjectTraitDefinition *trait,
                         ObjectStructDefinition *struct_def) {
  ObjectImpl *impl = (ObjectImpl *)obj_new(al, impl_size(trait), OBJECT_IMPL);
  impl->trait = trait;
  impl->struct_def = struct_def;

  size_t method_count = array_count(trait->method_names);
  for (size_t i = 0; i < method_count; i++) {
    impl->methods[i] = NULL;
  }
  return impl;
}

void obj_impl_free(ObjectImpl **obj, Allocator *al) {
  if (!*obj) {
    return;
  }
  al_free(al, *obj, impl_size((*obj)->trait));
  *obj = NULL;
}

ObjectTraitObject *obj_trait_object_new(Allocator *al,
                                        ObjectStructInstance *instance,
                                        ObjectImpl *impl) {
  ObjectTraitObject *trait_object =
      OBJECT_NEW(al, ObjectTraitObject, OBJECT_TRAIT_OBJECT);
  trait_object->instance = instance;
  trait_object->impl = impl;
  return trait_object;
}

void obj_trait_object_free(ObjectTraitObject **obj, Allocator *al) {
  if (!*obj) {
    return;
  }
  al_free(al, *obj, sizeof(ObjectTraitObject));
  *obj = NULL;
}

ObjectBoundMethod *obj_bound_method_new(Allocator *al, Value receiver,
                                        ObjectClosure *method) {
  ObjectBoundMethod *bound_method =
      OBJECT_NEW(al, ObjectBoundMethod, OBJECT_BOUND_METHOD);
  bound_method->receiver = receiver;
  bound_method->method = method;
  return bound_method;
}

void obj_bound_method_free(ObjectBoundMethod **obj, Allocator *al) {
  if (!*obj) {
    return;
  }
  al_free(al, *obj, sizeof(ObjectBoundMethod));
  *obj = NULL;
}

void obj_free(Object **obj, Allocator *al) {
  switch ((*obj)->type) {
  case OBJECT_STRING:
    obj_string_free((ObjectString **)obj, al);
    break;
  case OBJECT_FUNCTION:
    obj_function_free((ObjectFunction **)obj, al);
    break;
  case OBJECT_UPVALUE:
    obj_upvalue_free((ObjectUpvalue **)obj, al);
    break;
  case OBJECT_CLOSURE:
    obj_closure_free((ObjectClosure **)obj, al);
    break;
  case OBJECT_NATIVE:
    obj_native_free((ObjectNative **)obj, al);
    break;
  case OBJECT_STRUCT_DEFINITION:
    obj_struct_definition_free((ObjectStructDefinition **)obj, al);
    break;
  case OBJECT_STRUCT_INSTANCE:
    obj_struct_instance_free((ObjectStructInstance **)obj, al);
    break;
  case OBJECT_TRAIT_DEFINITION:
    obj_trait_definition_free((ObjectTraitDefinition **)obj, al);
    break;
  case OBJECT_IMPL:
    obj_impl_free((ObjectImpl **)obj, al);
    break;
  case OBJECT_TRAIT_OBJECT:
    obj_trait_object_free((ObjectTraitObject **)obj, al);
    break;
  case OBJECT_BOUND_METHOD:
    obj_bound_method_free((ObjectBoundMethod **)obj, al);
    break;
  default:
    assert(false && "unknown object type");
  }
}

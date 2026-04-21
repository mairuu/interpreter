#include "object.h"
#include "dynamic_array.h"
#include "memory.h"
#include "value.h"

#include <assert.h>
#include <chunk.h>
#include <stdio.h>
#include <string.h>

const char *OBJECT_TYPE_NAMES[] = {
    [OBJECT_STRING] = "STRING",
    [OBJECT_FUNCTION] = "FUNCTION",
    [OBJECT_UPVALUE] = "UPVALUE",
    [OBJECT_CLOSURE] = "CLOSURE",
    [OBJECT_NATIVE] = "NATIVE",
    [OBJECT_STRUCT_DEFINITION] = "STRUCT_DEFINITION",
    [OBJECT_STRUCT_INSTANCE] = "STRUCT_INSTANCE",
    [OBJECT_TRAIT_DEFINITION] = "TRAIT_DEFINITION",
    [OBJECT_IMPL] = "IMPL",
    [OBJECT_TRAIT_OBJECT] = "TRAIT_OBJECT",
    [OBJECT_BOUND_METHOD] = "BOUND_METHOD",
    [OBJECT_VARIANT_DEFINITION] = "VARIANT_DEFINITION",
    [OBJECT_VARIANT] = "VARIANT",
};

const char *object_type_to_string(ObjectType type) {
  if (type < 0 || type >= OBJECT_TYPE_COUNT) {
    return "UNKNOWN";
  }
  return OBJECT_TYPE_NAMES[type];
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

static inline size_t trait_definition_size(int method_count) {
  return sizeof(ObjectTraitDefinition) + sizeof(ObjectString *) * method_count;
}

ObjectTraitDefinition *obj_trait_definition_new(Allocator *al,
                                                ObjectString *name,
                                                uint32_t trait_id,
                                                int method_count) {
  ObjectTraitDefinition *trait = (ObjectTraitDefinition *)obj_new(
      al, trait_definition_size(method_count), OBJECT_TRAIT_DEFINITION);
  trait->name = name;
  trait->trait_id = trait_id;
  trait->method_count = method_count;
  for (int i = 0; i < method_count; i++) {
    trait->method_names[i] = NULL;
  }
  return trait;
}

void obj_trait_definition_free(ObjectTraitDefinition **obj, Allocator *al) {
  if (!*obj) {
    return;
  }

  al_free(al, *obj, trait_definition_size((*obj)->method_count));
  *obj = NULL;
}

int obj_trait_find_slot(ObjectTraitDefinition *trait,
                        ObjectString *method_name) {
  for (int i = 0; i < trait->method_count; i++) {
    if (obj_string_equals(trait->method_names[i], method_name)) {
      return i;
    }
  }
  return -1;
}

static inline size_t impl_size(ObjectTraitDefinition *trait) {
  return sizeof(ObjectImpl) + sizeof(Object *) * trait->method_count;
}

ObjectImpl *obj_impl_new(Allocator *al, ObjectTraitDefinition *trait,
                         ObjectStructDefinition *struct_def) {
  ObjectImpl *impl = (ObjectImpl *)obj_new(al, impl_size(trait), OBJECT_IMPL);
  impl->trait = trait;
  impl->struct_def = struct_def;

  for (int i = 0; i < trait->method_count; i++) {
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

ObjectTraitObject *obj_trait_object_new(Allocator *al, Object *receiver,
                                        ObjectImpl *impl) {
  ObjectTraitObject *trait_object =
      OBJECT_NEW(al, ObjectTraitObject, OBJECT_TRAIT_OBJECT);
  trait_object->receiver = receiver;
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

static inline size_t variant_definition_size(int arm_count) {
  return sizeof(ObjectVariantDefinition) + sizeof(VariantArm) * arm_count;
}

ObjectVariantDefinition *
obj_variant_definition_new(Allocator *al, ObjectString *name, int arm_count) {
  ObjectVariantDefinition *def = (ObjectVariantDefinition *)obj_new(
      al, variant_definition_size(arm_count), OBJECT_VARIANT_DEFINITION);
  def->name = name;
  def->arm_count = arm_count;
  for (int i = 0; i < arm_count; i++) {
    def->arms[i].name = NULL;
    def->arms[i].arity = 0;
  }
  return def;
}

void obj_variant_definition_free(ObjectVariantDefinition **obj, Allocator *al) {
  if (!*obj) {
    return;
  }
  al_free(al, *obj, variant_definition_size((*obj)->arm_count));
  *obj = NULL;
}

static inline size_t variant_instance_size(int arity) {
  return sizeof(ObjectVariant) + sizeof(Value) * arity;
}

ObjectVariant *obj_variant_new(Allocator *al, ObjectVariantDefinition *def,
                               int tag, int arity) {
  ObjectVariant *variant = (ObjectVariant *)obj_new(
      al, variant_instance_size(arity), OBJECT_VARIANT);
  variant->def = def;
  variant->tag = tag;
  variant->arity = arity;
  return variant;
}

void obj_variant_free(ObjectVariant **obj, Allocator *al) {
  if (!*obj) {
    return;
  }
  al_free(al, *obj, variant_instance_size((*obj)->arity));
  *obj = NULL;
}

ObjectArray *obj_array_new(Allocator *al) {
  ObjectArray *array = OBJECT_NEW(al, ObjectArray, OBJECT_ARRAY);
  array->length = 0;
  array->capacity = 0;
  array->values = NULL;
  return array;
}

void obj_array_free(ObjectArray **obj, Allocator *al) {
  if (!*obj) {
    return;
  }
  al_free(al, (*obj)->values, sizeof(Value) * (*obj)->capacity);
  al_free(al, *obj, sizeof(ObjectArray));
  *obj = NULL;
}

void obj_array_reserve(ObjectArray *array, int capacity, Allocator *al) {
  if (capacity <= array->capacity) {
    return;
  }
  int new_capacity = array->capacity == 0 ? 4 : array->capacity;
  while (new_capacity < capacity) {
    new_capacity *= 2;
  }

  if (new_capacity == array->capacity) {
    return;
  }

  Value *new_values = al_alloc(al, sizeof(Value) * new_capacity);
  if (array->values) {
    memcpy(new_values, array->values, sizeof(Value) * array->length);
    al_free(al, array->values, sizeof(Value) * array->capacity);
  }
  array->values = new_values;
  array->capacity = new_capacity;
}

bool obj_array_set(ObjectArray *array, int index, Value value) {
  if (index < 0 || index >= array->length) {
    return false;
  }
  array->values[index] = value;
  return true;
}

bool obj_array_push(ObjectArray *array, Value value, Allocator *al) {
  if (array->length >= array->capacity) {
    obj_array_reserve(array, array->length + 1, al);
  }
  array->values[array->length++] = value;
  return true;
}

Value obj_array_get(ObjectArray *array, int index) {
  if (index < 0 || index >= array->length) {
    return EMPTY_VALUE;
  }
  return array->values[index];
}

Value obj_array_pop(ObjectArray *array) {
  if (array->length == 0) {
    return EMPTY_VALUE;
  }
  return array->values[--array->length];
}

ObjectArrayIterator *obj_array_iterator_new(Allocator *al, ObjectArray *array) {
  ObjectArrayIterator *iter =
      OBJECT_NEW(al, ObjectArrayIterator, OBJECT_ARRAY_ITERATOR);
  iter->array = array;
  iter->index = 0;
  return iter;
}

void obj_array_iterator_free(ObjectArrayIterator **obj, Allocator *al) {
  if (!*obj) {
    return;
  }
  al_free(al, *obj, sizeof(ObjectArrayIterator));
  *obj = NULL;
}

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
  case OBJECT_TRAIT_OBJECT: {
    ObjectTraitObject *trait_object = (ObjectTraitObject *)obj;
    const char *for_struct = trait_object->impl->struct_def
                                 ? trait_object->impl->struct_def->name->chars
                                 : "<internal>";
    return snprintf(buf, size, "<trait object of %s for %s>",
                    trait_object->impl->trait->name->chars, for_struct);
  }
  case OBJECT_BOUND_METHOD:
    return snprintf(buf, size, "<fn %s>",
                    ((ObjectBoundMethod *)obj)->method->function->name->chars);
    break;
  case OBJECT_VARIANT_DEFINITION:
    return snprintf(buf, size, "<variant %s>",
                    ((ObjectVariantDefinition *)obj)->name->chars);
  case OBJECT_VARIANT:
    return snprintf(buf, size, "<%s.%s>",
                    ((ObjectVariant *)obj)->def->name->chars,
                    ((ObjectVariant *)obj)
                        ->def->arms[((ObjectVariant *)obj)->tag]
                        .name->chars);
  case OBJECT_ARRAY: {
    ObjectArray *array = (ObjectArray *)obj;
    return snprintf(buf, size, "<array [%d]>", array->length);
  }
  case OBJECT_ARRAY_ITERATOR: {
    ObjectArrayIterator *iter = (ObjectArrayIterator *)obj;
    return snprintf(buf, size, "<array.it [%d/%d]>", iter->index,
                    iter->array->length);
  }
  default:
    assert(false && "unknown object type");
  }
  return -1;
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
  case OBJECT_VARIANT_DEFINITION:
    obj_variant_definition_free((ObjectVariantDefinition **)obj, al);
    break;
  case OBJECT_VARIANT:
    obj_variant_free((ObjectVariant **)obj, al);
    break;
  case OBJECT_ARRAY:
    obj_array_free((ObjectArray **)obj, al);
    break;
  case OBJECT_ARRAY_ITERATOR:
    obj_array_iterator_free((ObjectArrayIterator **)obj, al);
    break;
  default:
    assert(false && "unknown object type");
  }
}
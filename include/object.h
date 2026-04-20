#pragma once

#include "chunk.h"
#include "hash_table.h"
#include "memory.h"
#include "value.h"
#include <stdint.h>

typedef enum {
  OBJECT_STRING,
  OBJECT_FUNCTION,
  OBJECT_UPVALUE,
  OBJECT_CLOSURE,
  OBJECT_NATIVE,
  OBJECT_STRUCT_DEFINITION,
  OBJECT_STRUCT_INSTANCE,
  OBJECT_TRAIT_DEFINITION,
  OBJECT_IMPL,
  OBJECT_TRAIT_OBJECT,
  OBJECT_BOUND_METHOD,
  OBJECT_VARIANT_DEFINITION,
  OBJECT_VARIANT,
} ObjectType;

struct Object {
  Object *next;
  ObjectType type;
  bool is_marked;
};

typedef struct {
  Object object;
  int length;
  char *chars;
  uint32_t hash;
  bool is_interned;
} ObjectString;

typedef struct ObjectTraitDefinition ObjectTraitDefinition;

typedef struct {
  Object object;
  int arity;
  int upvalue_count;
  Chunk chunk;
  ObjectString *name;
  ObjectTraitDefinition *constraints[]; // length is arity
} ObjectFunction;

typedef struct ObjectUpvalue {
  Object object;
  Value *location;
  Value closed;
  struct ObjectUpvalue *next;
} ObjectUpvalue;

typedef struct {
  Object object;
  ObjectFunction *function;
  int upvalue_count;
  ObjectUpvalue *upvalues[];
} ObjectClosure;

typedef struct VirtualMachine VirtualMachine;
typedef Value (*NativeFunc)(VirtualMachine *vm, int arg_count, Value *args);

typedef struct {
  Object object;
  NativeFunc function;
} ObjectNative;

typedef struct ObjectImpl ObjectImpl;

typedef struct {
  Object object;
  ObjectString *name; // for debugging and error messages
  uint16_t definition_id;
  HashTable fields; // field_name => field_index
  ObjectImpl **impls;
} ObjectStructDefinition;

typedef struct {
  Object obj;
  ObjectStructDefinition *def;
  Value fields[];
} ObjectStructInstance;

typedef struct ObjectTraitDefinition {
  Object object;
  ObjectString *name;
  uint16_t trait_id;
  int method_count;
  ObjectString *method_names[];
} ObjectTraitDefinition;

typedef struct ObjectImpl {
  Object object;
  ObjectTraitDefinition *trait;
  ObjectStructDefinition *struct_def;
  Object *methods[];
} ObjectImpl;

typedef struct {
  Object object;
  ObjectStructInstance *instance;
  ObjectImpl *impl;
} ObjectTraitObject;

typedef struct {
  Object object;
  Value receiver;
  ObjectClosure *method;
} ObjectBoundMethod;

typedef struct {
  ObjectString *name;
  int arity;
} VariantArm;

typedef struct {
  Object object;
  ObjectString *name;
  int arm_count;
  VariantArm arms[];
} ObjectVariantDefinition;

typedef struct {
  Object object;
  ObjectVariantDefinition *def;
  int tag;   // which arm
  int arity; // inlined payload length (from arm definition)
  Value payload[];
} ObjectVariant;

int obj_print(char *buf, size_t size, Object *obj);

static inline bool value_is_obj_type(Value value, ObjectType type) {
  return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

#define IS_STRING(value) value_is_obj_type(value, OBJECT_STRING)
#define IS_FUNCTION(value) value_is_obj_type(value, OBJECT_FUNCTION)
#define IS_UPVALUE(value) value_is_obj_type(value, OBJECT_UPVALUE)
#define IS_CLOSURE(value) value_is_obj_type(value, OBJECT_CLOSURE)
#define IS_NATIVE(value) value_is_obj_type(value, OBJECT_NATIVE)
#define IS_STRUCT_DEFINITION(value)                                            \
  value_is_obj_type(value, OBJECT_STRUCT_DEFINITION)
#define IS_STRUCT_INSTANCE(value)                                              \
  value_is_obj_type(value, OBJECT_STRUCT_INSTANCE)
#define IS_TRAIT_DEFINITION(value)                                             \
  value_is_obj_type(value, OBJECT_TRAIT_DEFINITION)
#define IS_IMPL(value) value_is_obj_type(value, OBJECT_IMPL)
#define IS_TRAIT_OBJECT(value) value_is_obj_type(value, OBJECT_TRAIT_OBJECT)
// #define IS_BOUND_METHOD(value) value_is_obj_type(value, OBJECT_BOUND_METHOD)
#define IS_RESULT(value) value_is_obj_type(value, OBJECT_RESULT)
#define IS_VARIANT_DEFINITION(value)                                           \
  value_is_obj_type(value, OBJECT_VARIANT_DEFINITION)
#define IS_VARIANT(value) value_is_obj_type(value, OBJECT_VARIANT)

#define AS_STRING(value) ((ObjectString *)AS_OBJECT(value))
#define AS_CSTRING(value) (((ObjectString *)AS_OBJECT(value))->chars)
#define AS_FUNCTION(value) ((ObjectFunction *)AS_OBJECT(value))
#define AS_UPVALUE(value) ((ObjectUpvalue *)AS_OBJECT(value))
#define AS_CLOSURE(value) ((ObjectClosure *)AS_OBJECT(value))
#define AS_NATIVE(value) ((ObjectNative *)AS_OBJECT(value))
#define AS_STRUCT_DEFINITION(value) ((ObjectStructDefinition *)AS_OBJECT(value))
#define AS_STRUCT_INSTANCE(value) ((ObjectStructInstance *)AS_OBJECT(value))
#define AS_TRAIT_DEFINITION(value) ((ObjectTraitDefinition *)AS_OBJECT(value))
#define AS_IMPL(value) ((ObjectImpl *)AS_OBJECT(value))
#define AS_TRAIT_OBJECT(value) ((ObjectTraitObject *)AS_OBJECT(value))
#define AS_RESULT(value) ((ObjectResult *)AS_OBJECT(value))
#define AS_VARIANT_DEFINITION(value)                                           \
  ((ObjectVariantDefinition *)AS_OBJECT(value))
#define AS_VARIANT(value) ((ObjectVariant *)AS_OBJECT(value))

// dispatch to the appropriate free function based on the object type
void obj_free(Object **obj, Allocator *al);

// for temporary strings, used for hashing and lookup, not heap allocated
ObjectString obj_string_create(const char *chars, int length, uint32_t hash);
// takes ownership of chars
ObjectString *obj_string_new(Allocator *al, char *chars, int length,
                             uint32_t hash);
void obj_string_free(ObjectString **obj, Allocator *al);
bool obj_string_equals(ObjectString *a, ObjectString *b);

ObjectFunction *obj_function_new(Allocator *al, int arity);
void obj_function_free(ObjectFunction **obj, Allocator *al);
bool obj_function_bind_constraint(ObjectFunction *function, int param_idx,
                                  ObjectTraitDefinition *trait);

ObjectUpvalue *obj_upvalue_new(Allocator *al, Value *location);
void obj_upvalue_free(ObjectUpvalue **obj, Allocator *al);

ObjectClosure *obj_closure_new(Allocator *al, ObjectFunction *function);
void obj_closure_free(ObjectClosure **obj, Allocator *al);

ObjectNative *obj_native_new(Allocator *al, NativeFunc function);
void obj_native_free(ObjectNative **obj, Allocator *al);

ObjectStructDefinition *obj_struct_definition_new(Allocator *al,
                                                  ObjectString *name,
                                                  uint32_t definition_id);
void obj_struct_definition_free(ObjectStructDefinition **obj, Allocator *al);
ObjectImpl *obj_struct_definition_find_impl(ObjectStructDefinition *def,
                                            ObjectTraitDefinition *trait);

ObjectStructInstance *obj_struct_instance_new(Allocator *al,
                                              ObjectStructDefinition *def);
void obj_struct_instance_free(ObjectStructInstance **obj, Allocator *al);

ObjectTraitDefinition *obj_trait_definition_new(Allocator *al,
                                                ObjectString *name,
                                                uint32_t trait_id,
                                                int method_count);
void obj_trait_definition_free(ObjectTraitDefinition **obj, Allocator *al);
int obj_trait_find_slot(ObjectTraitDefinition *trait,
                        ObjectString *method_name);

ObjectImpl *obj_impl_new(Allocator *al, ObjectTraitDefinition *trait,
                         ObjectStructDefinition *struct_def);
void obj_impl_free(ObjectImpl **obj, Allocator *al);

ObjectTraitObject *obj_trait_object_new(Allocator *al,
                                        ObjectStructInstance *instance,
                                        ObjectImpl *impl);
void obj_trait_object_free(ObjectTraitObject **obj, Allocator *al);

ObjectBoundMethod *obj_bound_method_new(Allocator *al, Value receiver,
                                        ObjectClosure *method);
void obj_bound_method_free(ObjectBoundMethod **obj, Allocator *al);

ObjectVariantDefinition *
obj_variant_definition_new(Allocator *al, ObjectString *name, int arm_count);
void obj_variant_definition_free(ObjectVariantDefinition **obj, Allocator *al);

ObjectVariant *obj_variant_new(Allocator *al, ObjectVariantDefinition *def,
                               int tag, int arity);
void obj_variant_free(ObjectVariant **obj, Allocator *al);
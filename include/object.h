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
  OBJECT_TRAIT_DEFINITION
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

typedef struct {
  Object object;
  int arity;
  int upvalue_count;
  Chunk chunk;
  ObjectString *name;
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
typedef Value (*NavtiveFunc)(VirtualMachine *vm, int arg_count, Value *args);

typedef struct {
  Object object;
  NavtiveFunc function;
} ObjectNative;

typedef struct {
  Object object;
  ObjectString *name; // for debugging and error messages
  uint16_t definition_id;
  HashTable fields; // field_name => field_index
} ObjectStructDefinition;

typedef struct {
  Object obj;
  ObjectStructDefinition *def;
  Value fields[];
} ObjectStructInstance;

typedef struct {
  Object object;
  ObjectString *name;
  uint16_t trait_id;
  ObjectString **method_names;
} ObjectTraitDefinition;

void object_print(Object *obj);

static inline bool value_is_object_type(Value value, ObjectType type) {
  return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

#define IS_STRING(value) value_is_object_type(value, OBJECT_STRING)
#define IS_FUNCTION(value) value_is_object_type(value, OBJECT_FUNCTION)
#define IS_UPVALUE(value) value_is_object_type(value, OBJECT_UPVALUE)
#define IS_CLOSURE(value) value_is_object_type(value, OBJECT_CLOSURE)
#define IS_NATIVE(value) value_is_object_type(value, OBJECT_NATIVE)
#define IS_STRUCT_DEFINITION(value)                                            \
  value_is_object_type(value, OBJECT_STRUCT_DEFINITION)
#define IS_STRUCT_INSTANCE(value)                                              \
  value_is_object_type(value, OBJECT_STRUCT_INSTANCE)
#define IS_TRAIT_DEFINITION(value)                                             \
  value_is_object_type(value, OBJECT_TRAIT_DEFINITION)

#define AS_STRING(value) ((ObjectString *)AS_OBJECT(value))
#define AS_FUNCTION(value) ((ObjectFunction *)AS_OBJECT(value))
#define AS_UPVALUE(value) ((ObjectUpvalue *)AS_OBJECT(value))
#define AS_CLOSURE(value) ((ObjectClosure *)AS_OBJECT(value))
#define AS_NATIVE(value) ((ObjectNative *)AS_OBJECT(value))
#define AS_STRUCT_DEFINITION(value) ((ObjectStructDefinition *)AS_OBJECT(value))
#define AS_STRUCT_INSTANCE(value) ((ObjectStructInstance *)AS_OBJECT(value))
#define AS_TRAIT_DEFINITION(value) ((ObjectTraitDefinition *)AS_OBJECT(value))

// dispatch to the appropriate free function based on the object type
void object_free(Object **obj, Allocator *al);

// for temporary strings, used for hashing and lookup, not heap allocated
ObjectString object_string_create(const char *chars, int length, uint32_t hash);
// takes ownership of chars
ObjectString *object_string_new(Allocator *al, char *chars, int length,
                                uint32_t hash);
void object_string_free(ObjectString **obj, Allocator *al);

ObjectFunction *object_function_new(Allocator *al);
void object_function_free(ObjectFunction **obj, Allocator *al);

ObjectUpvalue *object_upvalue_new(Allocator *al, Value *location);
void object_upvalue_free(ObjectUpvalue **obj, Allocator *al);

ObjectClosure *object_closure_new(Allocator *al, ObjectFunction *function);
void object_closure_free(ObjectClosure **obj, Allocator *al);

ObjectNative *object_native_new(Allocator *al, NavtiveFunc function);
void object_native_free(ObjectNative **obj, Allocator *al);

ObjectStructDefinition *object_struct_definition_new(Allocator *al,
                                                     ObjectString *name,
                                                     uint32_t definition_id);
void object_struct_definition_free(ObjectStructDefinition **obj, Allocator *al);

ObjectStructInstance *object_struct_instance_new(Allocator *al,
                                                 ObjectStructDefinition *def);
void object_struct_instance_free(ObjectStructInstance **obj, Allocator *al);

ObjectTraitDefinition *object_trait_definition_new(Allocator *al,
                                                   ObjectString *name,
                                                   uint32_t trait_id);
void object_trait_definition_free(ObjectTraitDefinition **obj, Allocator *al);
#pragma once

#include "chunk.h"
#include "memory.h"
#include "value.h"

typedef enum {
  OBJECT_STRING,
  OBJECT_FUNCTION,
  OBJECT_UPVALUE,
  OBJECT_CLOSURE,
  OBJECT_NATIVE
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

void object_print(Object *obj);

static inline bool value_is_object_type(Value value, ObjectType type) {
  return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

#define IS_STRING(value) value_is_object_type(value, OBJECT_STRING)
#define IS_FUNCTION(value) value_is_object_type(value, OBJECT_FUNCTION)
#define IS_UPVALUE(value) value_is_object_type(value, OBJECT_UPVALUE)
#define IS_CLOSURE(value) value_is_object_type(value, OBJECT_CLOSURE)
#define IS_NATIVE(value) value_is_object_type(value, OBJECT_NATIVE)

#define AS_STRING(value) ((ObjectString *)AS_OBJECT(value))
#define AS_FUNCTION(value) ((ObjectFunction *)AS_OBJECT(value))
#define AS_UPVALUE(value) ((ObjectUpvalue *)AS_OBJECT(value))
#define AS_CLOSURE(value) ((ObjectClosure *)AS_OBJECT(value))
#define AS_NATIVE(value) ((ObjectNative *)AS_OBJECT(value))

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
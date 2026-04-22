#pragma once

#include <setjmp.h>

#include "builtins.h"
#include "definition.h"
#include "hash_table.h"
#include "memory.h"
#include "object.h"
#include "value.h"

#define STACK_MAX 256
#define FRAMES_MAX 64

typedef struct {
  Value values[STACK_MAX];
  Value *top;

} ValueStack;

typedef struct {
  ObjectTraitDefinition *trait;
  ObjectImpl *impl;
} NativeImplEntry;

typedef struct {
  ObjectFunction *function;
  ObjectUpvalue **upvalues; // for closures

  uint8_t *ip;
  Value *base;
} CallFrame;

typedef struct VirtualMachine {
  ValueStack stack;
  CallFrame frames[FRAMES_MAX];
  int frame_count;

  HashTable strings;    // interned strings
  HashTable globals;    // global variables
  Object *objects;      // link-list of all allocated objects
  uint32_t gc_disabled; // gc is disabled when non-zero

  ObjectUpvalue *open_upvalues; // link-list of open upvalues
  NativeImplEntry *native_impls[OBJECT_TYPE_COUNT];

  Definition *definitions;     // dynamic array
  Object **definition_objects; // parallel dynamic array to definitions

  jmp_buf panic_jump;
  char panic_message[256];

  uint32_t next_trait_id;
  uint32_t next_definition_id;

  Allocator al;
  BuiltinRegistry builtins;
} VirtualMachine;

void vm_init(VirtualMachine *vm, Allocator al);
void vm_destroy(VirtualMachine *vm);

typedef struct {
  const char *name;
  NativeFunc fn;
} NativeMethodDef;

bool vm_register_native_impl(VirtualMachine *vm, ObjectType type,
                             ObjectTraitDefinition *trait,
                             const NativeMethodDef *methods, int count,
                             ObjectImpl **out_impl);

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR,
} InterpretResult;

InterpretResult vm_interpret(VirtualMachine *vm, const char *source);

void vm_runtime_error(VirtualMachine *vm, const char *format, ...)
    __attribute__((noreturn));

void vm_begin_staging(VirtualMachine *vm);
void vm_end_staging(VirtualMachine *vm);
void vm_track_object(VirtualMachine *vm, Object *object);

ObjectString *vm_intern_string(VirtualMachine *vm, const char *chars,
                               int length);

void vm_define_native(VirtualMachine *vm, const char *name,
                      NativeFunc function);
void vm_define_global(VirtualMachine *vm, const char *name, Value value);
void vm_undefine_global(VirtualMachine *vm, const char *name);

ObjectString *vm_new_string(VirtualMachine *vm, char *chars, int length,
                            uint32_t hash);

ObjectFunction *vm_new_function(VirtualMachine *vm, int arity);

ObjectUpvalue *vm_new_upvalue(VirtualMachine *vm, Value *location);

ObjectClosure *vm_new_closure(VirtualMachine *vm, ObjectFunction *function);

ObjectNative *vm_new_native(VirtualMachine *vm, NativeFunc function);

ObjectStructDefinition *vm_new_struct_definition(VirtualMachine *vm,
                                                 ObjectString *name,
                                                 uint16_t definition_id);

ObjectStructInstance *vm_new_struct_instance(VirtualMachine *vm,
                                             ObjectStructDefinition *def);

ObjectTraitDefinition *vm_new_trait_definition(VirtualMachine *vm,
                                               ObjectString *name,
                                               uint16_t trait_id,
                                               int method_count);

ObjectImpl *vm_new_impl(VirtualMachine *vm, ObjectTraitDefinition *trait,
                        ObjectStructDefinition *struct_def);

ObjectTraitObject *vm_new_trait_object(VirtualMachine *vm, Object *receiver,
                                       ObjectImpl *impl);

ObjectVariantDefinition *vm_new_variant_definition(VirtualMachine *vm,
                                                   ObjectString *name,
                                                   int arm_count);

ObjectVariant *vm_new_variant(VirtualMachine *vm, ObjectVariantDefinition *def,
                              int tag, int arity);

ObjectArray *vm_new_array(VirtualMachine *vm);

ObjectArrayIterator *vm_new_array_iterator(VirtualMachine *vm,
                                           ObjectArray *array);

ObjectMap *vm_new_map(VirtualMachine *vm);
ObjectMapIterator *vm_new_map_iterator(VirtualMachine *vm, ObjectMap *map,
                                       ObjectArray *keys);
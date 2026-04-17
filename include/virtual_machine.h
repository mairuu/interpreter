#pragma once

#include <setjmp.h>

#include "builtins.h"
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
  ObjectFunction *function;
  ObjectUpvalue **upvalues; // for closures

  uint8_t *ip;
  Value *base;
} CallFrame;

typedef struct VirtualMachine {
  ValueStack stack;
  CallFrame frames[FRAMES_MAX];
  int frame_count;

  HashTable strings; // interned strings
  HashTable globals; // global variables
  Object *objects;   // link-list of all allocated objects

  ObjectUpvalue *open_upvalues; // link-list of open upvalues

  jmp_buf panic_jump;
  char panic_message[256];

  Allocator al;
  BuiltinRegistry builtins;
} VirtualMachine;

void vm_init(VirtualMachine *vm, Allocator al);
void vm_destroy(VirtualMachine *vm);

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR,
} InterpretResult;

InterpretResult vm_interpret(VirtualMachine *vm, const char *source);

void vm_runtime_error(VirtualMachine *vm, const char *format, ...);

ObjectString *vm_intern_string(VirtualMachine *vm, const char *chars,
                               int length);

void vm_define_native(VirtualMachine *vm, const char *name,
                      NavtiveFunc function);

ObjectString *vm_new_string(VirtualMachine *vm, char *chars, int length,
                            uint32_t hash);

ObjectFunction *vm_new_function(VirtualMachine *vm, int arity);

ObjectUpvalue *vm_new_upvalue(VirtualMachine *vm, Value *location);

ObjectClosure *vm_new_closure(VirtualMachine *vm, ObjectFunction *function);

ObjectNative *vm_new_native(VirtualMachine *vm, NavtiveFunc function);

ObjectStructDefinition *vm_new_struct_definition(VirtualMachine *vm,
                                                 ObjectString *name,
                                                 uint16_t definition_id);

ObjectStructInstance *vm_new_struct_instance(VirtualMachine *vm,
                                             ObjectStructDefinition *def);

ObjectTraitDefinition *vm_new_trait_definition(VirtualMachine *vm,
                                               ObjectString *name,
                                               uint16_t trait_id);

ObjectImpl *vm_new_impl(VirtualMachine *vm, ObjectTraitDefinition *trait,
                        ObjectStructDefinition *struct_def);

ObjectTraitObject *vm_new_trait_object(VirtualMachine *vm,
                                       ObjectStructInstance *instance,
                                       ObjectImpl *impl);
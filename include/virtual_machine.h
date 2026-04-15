#pragma once

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

  // for native functions to return type names
  ObjectString *type_nil;
  ObjectString *type_bool;
  ObjectString *type_number;
  ObjectString *type_object;
  ObjectString *type_empty;

  bool in_panic;
  Allocator al;
} VirtualMachine;

void vm_init(VirtualMachine *vm, Allocator al);
void vm_destroy(VirtualMachine *vm);

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR,
} InterpretResult;

InterpretResult vm_interpret(VirtualMachine *vm, const char *source);
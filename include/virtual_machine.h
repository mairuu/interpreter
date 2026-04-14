#pragma once

#include "chunk.h"
#include "hash_table.h"
#include "memory.h"
#include "value.h"

#define STACK_MAX 256

typedef struct {
  Value values[STACK_MAX];
  Value *top;
} ValueStack;

typedef struct {
  Chunk chunk;
  ValueStack stack;

  HashTable strings; // interned strings
  HashTable globals;
  Object *objects; // all allocated objects

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
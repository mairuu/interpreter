#pragma once

#include "object.h"

typedef struct {
  const char *name;
  NativeFunc fn;
} BuiltinDef;

typedef struct {
  ObjectString *type_bool;
  ObjectString *type_nil;
  ObjectString *type_number;
  ObjectString *type_object;
  ObjectString *type_empty;
  ObjectString *type_string;

  ObjectTraitDefinition *iterable;
} BuiltinRegistry;

void builtins_init(BuiltinRegistry *reg, VirtualMachine *vm);
void builtins_register(BuiltinRegistry *reg, VirtualMachine *vm);
void builtins_destroy(BuiltinRegistry *reg);
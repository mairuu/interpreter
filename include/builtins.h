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

  // populated by bootstrap code
  ObjectTraitDefinition *iterable;
  ObjectTraitDefinition *into_iterable;

  ObjectVariantDefinition *result;

  ObjectTraitDefinition *array;
  ObjectImpl *array_impl_obj_array;

  ObjectTraitDefinition *map;
  ObjectStructDefinition *map_entry;
  ObjectImpl *map_impl_obj_map;
} BuiltinRegistry;

void builtins_init(BuiltinRegistry *reg, VirtualMachine *vm);
void builtins_register(BuiltinRegistry *reg, VirtualMachine *vm);
void builtins_destroy(BuiltinRegistry *reg);
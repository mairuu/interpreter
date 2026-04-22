#pragma once

#include "string_utils.h"

typedef enum {
  DEFKIND_STRUCT,
  DEFKIND_TRAIT,
  DEFKIND_VARIANT,
} DefinitionKind;

typedef struct {
  String *fields;
} StructDefinition;

typedef struct {
  String *methods;
} TraitDefinition;

typedef struct {
  String name;
  String *fields;
} VariantArm;

typedef struct {
  VariantArm *arms;
} VariantDefinition;

typedef struct {
  DefinitionKind kind;
  String name;
  union {
    StructDefinition struct_def;
    TraitDefinition trait_def;
    VariantDefinition variant_def;
  } as;
} Definition;

const char *definition_kind_name(DefinitionKind kind);

void definition_destroy(Definition *def, Allocator *al);
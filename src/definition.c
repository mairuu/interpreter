#include "definition.h"

#include <assert.h>

#include "dynamic_array.h"
#include "string_utils.h"

const char *DEFINITION_KIND_NAMES[] = {
    [DEFKIND_STRUCT] = "struct",
    [DEFKIND_TRAIT] = "trait",
    [DEFKIND_VARIANT] = "variant",
};

const char *definition_kind_name(DefinitionKind kind) {
  if (kind < 0 || kind >= sizeof(DEFINITION_KIND_NAMES) /
                              sizeof(DEFINITION_KIND_NAMES[0])) {
    return "UNKNOWN";
  }
  return DEFINITION_KIND_NAMES[kind];
}

void definition_destroy(Definition *def, Allocator *al) {
  str_destroy(&def->name, al);

  switch (def->kind) {
  case DEFKIND_STRUCT: {
    StructDefinition *struct_def = &def->as.struct_def;
    int field_count = array_count(struct_def->fields);
    for (int i = 0; i < field_count; i++) {
      str_destroy(&struct_def->fields[i], al);
    }
    array_free(struct_def->fields, al);
    break;
  }
  case DEFKIND_TRAIT: {
    TraitDefinition *trait_def = &def->as.trait_def;
    int method_count = array_count(trait_def->methods);
    for (int i = 0; i < method_count; i++) {
      str_destroy(&trait_def->methods[i], al);
    }
    array_free(trait_def->methods, al);
    break;
  }
  case DEFKIND_VARIANT: {
    VariantDefinition *variant_def = &def->as.variant_def;
    int arm_count = array_count(variant_def->arms);
    for (int i = 0; i < arm_count; i++) {
      VariantArm *arm = &variant_def->arms[i];
      str_destroy(&arm->name, al);
      int field_count = array_count(arm->fields);
      for (int j = 0; j < field_count; j++) {
        str_destroy(&arm->fields[j], al);
      }
      array_free(arm->fields, al);
    }
    array_free(variant_def->arms, al);
    break;
  }
  default:
    assert(false && "unexpected definition kind");
    break;
  }
}
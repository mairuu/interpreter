#pragma once

// internal usage for compiler

#include <stdbool.h>
#include <stdint.h>

#include "definition.h"
#include "memory.h"
#include "scanner.h"
#include "string_utils.h"

#define DEFTABLE_INITIAL_CAPACITY 8

typedef struct {
  StringView name;
  Token name_token;
  int index; // index into the definitions array
} DefinitionEntry;

typedef struct {
  Definition *definitions;
  DefinitionEntry *entries;
  int count;
  int capacity;
} DefinitionTable;

// shallow copy of definitions array and seed the lookup table
void deftable_init(DefinitionTable *table, Definition *definitions,
                   Allocator *al);

// release internal resources
void deftable_destroy(DefinitionTable *table, Allocator *al);

// move ownership of definitions array to caller and release internal resources.
Definition *deftable_claim(DefinitionTable *table, Allocator *al);

DefinitionEntry *deftable_get(DefinitionTable *table, StringView name);
Definition *deftable_get_def(DefinitionTable *table, StringView name);

bool deftable_put(DefinitionTable *table, DefinitionEntry entry, Allocator *al);

// void deftable_delete(DefinitionTable *table, StringView name, Allocator *al);

int deftable_register(DefinitionTable *table, Token *name_token, Definition def,
                      Allocator *al);

typedef struct {
  DefinitionEntry *entry;

  DefinitionTable *_table;
  int _index;
} DefinitionTableIterator;

void deftable_iter_init(DefinitionTableIterator *iter, DefinitionTable *table);
bool deftable_iter_next(DefinitionTableIterator *iter);
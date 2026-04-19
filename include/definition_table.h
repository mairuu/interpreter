#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "memory.h"
#include "scanner.h"
#include "string_utils.h"

#define DEFTABLE_INITIAL_CAPACITY 8

typedef enum {
  DEFKIND_STRUCT,
  DEFKIND_TRAIT,
  DEFKIND_VARIANT,
} DefinitionKind;

typedef struct {
  String name; // The entry owns this String allocation
  DefinitionKind kind;
  Token name_token;   // for error reporting
  int constant_index; // into the constant pool
} DefinitionEntry;

typedef struct {
  DefinitionEntry *entries;
  int count; // count of used slots (active + tombstones)
  int capacity;
} DefinitionTable;

const char *definition_kind_name(DefinitionKind kind);

void deftable_init(DefinitionTable *table, Allocator *al);

// Destroys the table AND frees the strings owned by all active entries
void deftable_destroy(DefinitionTable *table, Allocator *al);

// Retrieves an entry using a StringView to avoid allocations
DefinitionEntry *deftable_get(DefinitionTable *table, StringView name);

// Inserts an entry. The table takes ownership of entry.name.
// If an entry with the same name already exists, the OLD string is freed.
bool deftable_put(DefinitionTable *table, DefinitionEntry entry, Allocator *al);

// Deletes an entry by name and frees the underlying String.
// Requires the allocator.
void deftable_delete(DefinitionTable *table, StringView name, Allocator *al);

// Iterator
typedef struct {
  DefinitionEntry *entry;

  DefinitionTable *_table;
  int _index;
} DefinitionTableIterator;

void deftable_iter_init(DefinitionTableIterator *iter, DefinitionTable *table);
bool deftable_iter_next(DefinitionTableIterator *iter);
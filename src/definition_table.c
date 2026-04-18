#include "definition_table.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "memory.h"

#define TOMBSTONE_CHARS ((char *)-1)
#define HIGH_WATER_MARK 0.75
#define LOW_WATER_MARK 0.5

void deftable_init(DefinitionTable *table, Allocator *al) {
  (void)al; // unused
  table->count = 0;
  table->capacity = 0;
  table->entries = NULL;
}

static inline bool is_empty(DefinitionEntry *entry) {
  return entry->name.chars == NULL;
}

static inline bool is_tombstone(DefinitionEntry *entry) {
  return entry->name.chars == TOMBSTONE_CHARS;
}

static inline void mark_empty(DefinitionEntry *entry) {
  entry->name.chars = NULL;
  entry->name.length = 0;
}

static inline void mark_tombstone(DefinitionEntry *entry) {
  entry->name.chars = TOMBSTONE_CHARS;
  entry->name.length = 0;
}

static inline bool string_equals_view(String a, StringView b) {
  if (a.length != b.length)
    return false;
  if (a.length == 0)
    return true;
  return memcmp(a.chars, b.chars, a.length) == 0;
}

void deftable_destroy(DefinitionTable *table, Allocator *al) {
  if (table->capacity > 0) {
    for (int i = 0; i < table->capacity; i++) {
      DefinitionEntry *entry = &table->entries[i];
      if (!is_empty(entry) && !is_tombstone(entry)) {
        str_destroy(&entry->name, al);
      }
    }
    al_free(al, table->entries, sizeof(*(table->entries)) * table->capacity);
  }
  deftable_init(table, al);
}

// Core probe loop. Returns the target entry, a tombstone we can recycle, or an
// empty slot.
static DefinitionEntry *find_slot(DefinitionTable *table, StringView name) {
  uint32_t hash = hash_string(name.chars, name.length);
  DefinitionEntry *tombstone = NULL;

  for (int i = 0; i < table->capacity; i++) {
    uint32_t index = (hash + i) & (table->capacity - 1);
    DefinitionEntry *entry = &table->entries[index];

    if (is_empty(entry)) {
      return tombstone != NULL ? tombstone : entry;
    }

    if (!is_tombstone(entry) && string_equals_view(entry->name, name)) {
      return entry; // found exact match
    }

    if (is_tombstone(entry) && tombstone == NULL) {
      tombstone =
          entry; // mark first tombstone to reuse if we don't find the key
    }
  }

  assert(false && "hash table is full, should have been resized");
  return NULL;
}

DefinitionEntry *deftable_get(DefinitionTable *table, StringView name) {
  if (table->capacity == 0)
    return NULL;

  DefinitionEntry *entry = find_slot(table, name);
  if (entry == NULL || is_empty(entry) || is_tombstone(entry)) {
    return NULL;
  }
  return entry;
}

static void _deftable_alloc(DefinitionTable *table, int capacity,
                            Allocator *al) {
  table->count = 0;
  table->entries = al_alloc(al, sizeof(DefinitionEntry) * capacity);
  table->capacity = capacity;
  for (int i = 0; i < capacity; i++) {
    mark_empty(&table->entries[i]);
  }
}

static void _deftable_grow_table(DefinitionTable *table, Allocator *al) {
  int key_count = 0;
  for (int i = 0; i < table->capacity; i++) {
    DefinitionEntry *entry = &table->entries[i];
    if (!is_empty(entry) && !is_tombstone(entry)) {
      key_count++;
    }
  }

  int new_capacity = table->capacity;
  while (key_count >= new_capacity * LOW_WATER_MARK) {
    new_capacity *= 2;
  }
  assert(new_capacity > 0);

  DefinitionTable new_table;
  _deftable_alloc(&new_table, new_capacity, al);

  // Transfer ownership of entries to the new table
  for (int i = 0; i < table->capacity; i++) {
    DefinitionEntry *entry = &table->entries[i];
    if (is_empty(entry) || is_tombstone(entry)) {
      continue;
    }

    StringView view = {entry->name.chars, entry->name.length};
    DefinitionEntry *dest = find_slot(&new_table, view);
    *dest = *entry;    // direct struct copy transfers ownership
    new_table.count++; // track used slots in the new table
  }

  // Free the old entries array directly; do NOT use deftable_destroy
  // because new_table now owns the strings.
  al_free(al, table->entries, sizeof(*(table->entries)) * table->capacity);

  assert(new_table.count == key_count);
  *table = new_table;
}

bool deftable_put(DefinitionTable *table, DefinitionEntry entry,
                  Allocator *al) {
  if (table->capacity == 0) {
    _deftable_alloc(table, DEFTABLE_INITIAL_CAPACITY, al);
  } else if (table->count >= table->capacity * HIGH_WATER_MARK) {
    _deftable_grow_table(table, al);
  }

  StringView view = {entry.name.chars, entry.name.length};
  DefinitionEntry *target = find_slot(table, view);

  if (target == NULL) {
    return false;
  }

  if (is_empty(target)) {
    table->count++; // only increment for truly empty slots, not tombstones
  } else if (!is_tombstone(target)) {
    // We are overwriting an existing active entry.
    // We MUST free its old string to prevent memory leaks.
    str_destroy(&target->name, al);
  }

  // target now safely takes ownership of the new entry payload
  *target = entry;
  return true;
}

void deftable_delete(DefinitionTable *table, StringView name, Allocator *al) {
  if (table->capacity == 0)
    return;

  DefinitionEntry *entry = find_slot(table, name);
  if (entry == NULL || is_empty(entry) || is_tombstone(entry)) {
    return; // nothing to delete
  }

  // Free the string being removed
  str_destroy(&entry->name, al);
  mark_tombstone(entry);
}

void deftable_iter_init(DefinitionTableIterator *iter, DefinitionTable *table) {
  assert(table != NULL);
  iter->entry = NULL;
  iter->_table = table;
  iter->_index = -1;
}

bool deftable_iter_next(DefinitionTableIterator *iter) {
  if (iter->_table == NULL) {
    return false;
  }

  while (++iter->_index < iter->_table->capacity) {
    DefinitionEntry *entry = &iter->_table->entries[iter->_index];
    if (is_empty(entry) || is_tombstone(entry)) {
      continue;
    }

    iter->entry = entry;
    return true;
  }

  return false;
}
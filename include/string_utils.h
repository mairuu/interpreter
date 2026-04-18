#pragma once

#include "memory.h"

#include <stdint.h>

typedef struct {
  char *chars;
  int length;
  int capacity;
} String;

String str_empty(void);
// String takes ownership of chars
String str_create(char *chars, int length);
// String copies the chars into a new allocation
String str_from_str(const char* chars, int length, Allocator *al);
// String copies the cstr into a new allocation
String str_from_cstr(const char *cstr, Allocator *al);

// String str_copy(String str, Allocator *al);

void str_destroy(String *str, Allocator *al);

bool str_is_empty(String str);

uint32_t hash_string(const char *str, int length);

char *copy_escaped_string(const char *str, int length, int *out_length,
                                 int *out_capacity, Allocator *al);
char *copy_string(const char *str, int length, Allocator *al);
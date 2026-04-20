#include "string_utils.h"

#include <string.h>

String str_empty(void) { return (String){NULL, 0, 0}; }

String str_create(char *chars, int length) {
  return (String){chars, length, length + 1};
}

String str_from_str(const char *chars, int length, Allocator *al) {
  char *copy = copy_string(chars, length, al);
  return (String){copy, length, length + 1};
}

String str_from_cstr(const char *cstr, Allocator *al) {
  int length = strlen(cstr);
  return str_from_str(cstr, length, al);
}

void str_destroy(String *str, Allocator *al) {
  if (str->chars == NULL) {
    return;
  }
  al_free(al, str->chars, str->capacity);
  str->chars = NULL;
  str->length = 0;
  str->capacity = 0;
}

bool str_is_empty(String str) { return str.length == 0; }

StringView sv_create(const char *chars, int length) {
  return (StringView){chars, length};
}

StringView sv_from_str(String str) {
  return sv_create(str.chars, str.length);
}

bool sv_equals(StringView a, StringView b) {
  if (a.length != b.length) {
    return false;
  }
  return memcmp(a.chars, b.chars, a.length) == 0;
}

uint32_t hash_string(const char *str, int length) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < length; i++) {
    hash ^= (uint8_t)str[i];
    hash *= 16777619u;
  }
  return hash;
}

char *copy_string(const char *str, int length, Allocator *al) {
  char *copy = al_alloc(al, length + 1);
  memcpy(copy, str, length);
  copy[length] = '\0';
  return copy;
}

char *copy_escaped_string(const char *str, int length, int *out_length,
                          int *out_capacity, Allocator *al) {
  int capacity = length;
  char *chars = al_alloc(al, capacity + 1);
  int j = 0;
  for (int i = 0; i < length; i++) {
    if (str[i] == '\\' && i + 1 < length) {
      i++;
      switch (str[i]) {
      case 'n':
        chars[j++] = '\n';
        break;
      case 'r':
        chars[j++] = '\r';
        break;
      case 't':
        chars[j++] = '\t';
        break;
      case '\\':
        chars[j++] = '\\';
        break;
      case '"':
        chars[j++] = '"';
        break;
      default:
        chars[j++] = str[i];
        break;
      }
    } else {
      chars[j++] = str[i];
    }
  }
  chars[j] = '\0';
  if (out_length) {
    *out_length = j;
  }
  if (out_capacity) {
    *out_capacity = capacity;
  }
  return chars;
}
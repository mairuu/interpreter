#include "value.h"
#include "object.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

bool value_is_falsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static bool string_equals(ObjectString *a, ObjectString *b) {
  if (a->is_interned && b->is_interned) {
    return a == b;
  }
  if (a->length != b->length)
    return false;
  return memcmp(a->chars, b->chars, a->length) == 0;
}

bool value_equals(Value a, Value b) {
  if (a.type != b.type) {
    return false;
  }

  switch (a.type) {
  case VALUE_NIL:
    return true;
  case VALUE_BOOL:
    return AS_BOOL(a) == AS_BOOL(b);
  case VALUE_NUMBER:
    return AS_NUMBER(a) == AS_NUMBER(b);
  case VALUE_OBJECT:
    if (a.as.object->type == OBJECT_STRING &&
        b.as.object->type == OBJECT_STRING) {
      return string_equals(AS_STRING(a), AS_STRING(b));
    }
    return AS_OBJECT(a) == AS_OBJECT(b);
  case VALUE_EMPTY:
    return true;
  }

  assert(false && "invalid value type");
  return false; // unreachable
}

void value_print(Value value) {
  switch (value.type) {
  case VALUE_NIL:
    printf("nil");
    break;
  case VALUE_BOOL:
    printf(AS_BOOL(value) ? "true" : "false");
    break;
  case VALUE_NUMBER:
    printf("%g", AS_NUMBER(value));
    break;
  case VALUE_OBJECT:
    object_print(AS_OBJECT(value));
    break;
  case VALUE_EMPTY:
    printf("<empty>");
    break;
  default:
    assert(false && "invalid value type");
  }
}
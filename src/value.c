#include "value.h"
#include "object.h"

#include <assert.h>
#include <stdio.h>

bool value_is_falsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
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
      return obj_string_equals(AS_STRING(a), AS_STRING(b));
    }
    return AS_OBJECT(a) == AS_OBJECT(b);
  case VALUE_EMPTY:
    return true;
  }

  assert(false && "invalid value type");
  return false; // unreachable
}

int value_print(char *buf, size_t size, Value value) {
  switch (value.type) {
  case VALUE_NIL:
    return snprintf(buf, size, "nil");
  case VALUE_BOOL:
    return snprintf(buf, size, "%s", AS_BOOL(value) ? "true" : "false");
  case VALUE_NUMBER:
    return snprintf(buf, size, "%g", AS_NUMBER(value));
  case VALUE_OBJECT:
    return obj_print(buf, size, AS_OBJECT(value));
  case VALUE_EMPTY:
    return snprintf(buf, size, "<empty>");
  default:
    assert(false && "invalid value type");
  }
  return -1;
}
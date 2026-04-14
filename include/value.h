#pragma once

#include <stdint.h>

typedef enum {
  VALUE_NIL,
  VALUE_BOOL,
  VALUE_NUMBER,
  VALUE_OBJECT,
  VALUE_EMPTY // internal use
} ValueType;

typedef struct Object Object;

typedef struct {
  ValueType type;
  union {
    bool boolean;
    double number;
    Object *object;
  } as;
} Value;

#define IS_NIL(value) ((value).type == VALUE_NIL)
#define IS_BOOL(value) ((value).type == VALUE_BOOL)
#define IS_NUMBER(value) ((value).type == VALUE_NUMBER)
#define IS_OBJECT(value) ((value).type == VALUE_OBJECT)
#define IS_EMPTY(value) ((value).type == VALUE_EMPTY)

#define AS_NIL(value) ((value))
#define AS_BOOL(value) ((value).as.boolean)
#define AS_NUMBER(value) ((value).as.number)
#define AS_OBJECT(value) ((value).as.object)

#define NIL_VALUE ((Value){.type = VALUE_NIL})
#define BOOL_VALUE(value) ((Value){.type = VALUE_BOOL, .as.boolean = value})
#define NUMBER_VALUE(value) ((Value){.type = VALUE_NUMBER, .as.number = value})
#define OBJECT_VALUE(obj)                                                      \
  ((Value){.type = VALUE_OBJECT, .as.object = (Object *)obj})
#define EMPTY_VALUE ((Value){.type = VALUE_EMPTY})

bool value_is_falsey(Value value);
bool value_equals(Value a, Value b);

void value_print(Value value);

#pragma once

#include "definition.h"
#include "memory.h"
#include "string_utils.h"

#include <stdint.h>

typedef enum {
  RAW_NIL,
  RAW_BOOL,
  RAW_NUMBER,
  RAW_STRING,
  RAW_FUNC,
} RawType;

typedef struct Proto Proto;

typedef struct RawConstant {
  RawType type;
  union {
    bool boolean;
    double number;
    String string;
    Proto *proto;
  } as;
} RawConstant;

#define RAW_NIL_VALUE ((RawConstant){.type = RAW_NIL})
#define RAW_BOOL_VALUE(b) ((RawConstant){.type = RAW_BOOL, .as.boolean = (b)})
#define RAW_NUMBER_VALUE(n)                                                    \
  ((RawConstant){.type = RAW_NUMBER, .as.number = (n)})
#define RAW_STRING_VALUE(s, l, c)                                              \
  ((RawConstant){.type = RAW_STRING,                                           \
                 .as.string = {.chars = (s), .length = (l), .capacity = (c)}})
#define RAW_FUNC_VALUE(p) ((RawConstant){.type = RAW_FUNC, .as.proto = (p)})

// type of function prototypes.
typedef enum { PROTO_SCRIPT, PROTO_FUNCTION, PROTO_METHOD } ProtoType;

// function prototype.
typedef struct Proto {
  ProtoType type;
  String name;

  int arity;
  int upvalue_count;

  uint8_t *code;
  RawConstant *constants;
  int *lines;
} Proto;

void proto_destroy(Proto *proto, Allocator *al);

// return a compiled prototype, and definitions
// out_def is shallow copy of in_def with new definitions added by the source,
// *caller should free it after use*
Proto *compile(const char *source, Definition *in_def, Definition **out_def,
               Allocator *al);
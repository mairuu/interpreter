#pragma once

#include "memory.h"
#include <stdint.h>

typedef enum { RAW_NIL, RAW_BOOL, RAW_NUMBER, RAW_STRING, RAW_FUNC } RawType;

typedef struct Proto Proto;

typedef struct RawConstant {
  RawType type;
  union {
    bool boolean;
    double number;
    struct {
      char *chars;
      int length;
      int capacity;
    } string;
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
typedef enum { PROTO_SCRIPT, PROTO_FUNCTION } ProtoType;

// function prototype.
typedef struct Proto {
  ProtoType type;
  char *name;

  int arity;

  uint8_t *code;
  RawConstant *constants;
  int *lines;
} Proto;

// void proto_init(Proto *proto, const char *name, Allocator *al);

void proto_destroy(Proto *proto, Allocator *al);

// void proto_write_byte(Proto *proto, uint8_t byte, int line, Allocator *al);

// int proto_write_constant(Proto *proto, RawConstant constant, Allocator *al);

Proto *compile(const char *source, Allocator *al);
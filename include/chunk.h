#pragma once

#include "memory.h"
#include "value.h"

typedef struct Proto Proto;
typedef struct RawConstant RawConstant;

typedef struct Chunk {
  uint8_t *instructions;
  Value *constants;
  int *lines;
} Chunk;

typedef struct ConstantLoader ConstantLoader;

typedef Value (*LoadConstantFunc)(ConstantLoader *loader, Allocator *al,
                                  const RawConstant *raw_constant);

typedef struct ConstantLoader {
  LoadConstantFunc load_constant;
  void *ctx;
} ConstantLoader;

void chunk_init(Chunk *chunk);
void chunk_destroy(Chunk *chunk, Allocator *al);

void chunk_load_from_proto(Chunk *chunk, const Proto *proto,
                           ConstantLoader *loader, Allocator *al);
#include "chunk.h"
#include "compiler.h"
#include "dynamic_array.h"
#include "memory.h"

void chunk_init(Chunk *chunk) {
  chunk->instructions = NULL;
  chunk->constants = NULL;
  chunk->lines = NULL;
}

void chunk_destroy(Chunk *chunk, Allocator *al) {
  if (chunk->instructions) {
    array_free(chunk->instructions, al);
  }
  if (chunk->constants) {
    array_free(chunk->constants, al);
  }
  if (chunk->lines) {
    array_free(chunk->lines, al);
  }
}

void chunk_load_from_proto(Chunk *chunk, const Proto *proto,
                           ConstantLoader *loader, Allocator *al) {
  chunk_destroy(chunk, al);

  chunk->instructions = array_copy(proto->code, al);
  chunk->lines = array_copy(proto->lines, al);

  size_t constant_count = array_count(proto->constants);
  array_reserve(chunk->constants, constant_count, al);

  for (size_t i = 0; i < constant_count; i++) {
    array_push(chunk->constants,
               loader->load_constant(loader, al, &proto->constants[i]), al);
  }
}
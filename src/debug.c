#include "debug.h"

#include <stdio.h>

#include "chunk.h"
#include "dynamic_array.h"
#include "object.h"
#include "opcode.h"

static int simple_instruction(const char *name, int offset) {
  printf("%-16s\n", name);
  return offset + 1;
}

static int constant_instruction(const char *name, Chunk *chunk, int offset) {
  uint8_t constant_index = chunk->instructions[offset + 1];
  printf("%-16s %d '", name, constant_index);
  Value constant = chunk->constants[constant_index];
  value_print(constant);
  printf("'\n");
  return offset + 2;
}

static int byte_instruction(const char *name, Chunk *chunk, int offset) {
  uint8_t slot = chunk->instructions[offset + 1];
  printf("%-16s %d\n", name, slot);
  return offset + 2;
}

static int jump_instruction(const char *name, Chunk *chunk, bool is_forward,
                            int offset) {
  uint16_t jump = (uint16_t)(chunk->instructions[offset + 1] << 8);
  jump |= chunk->instructions[offset + 2];
  printf("%-16s %04d -> %04d\n", name, offset,
         offset + 3 + (is_forward ? jump : -jump));
  return offset + 3;
}

static int field_instruction(const char *name, Chunk *chunk, int offset) {
  // [op_code, name_constant, def_id, def_id, offset]
  uint8_t name_constant = chunk->instructions[offset + 1];
  uint16_t def_id = (uint16_t)(chunk->instructions[offset + 2] << 8) |
                    chunk->instructions[offset + 3];
  uint8_t offset_val = chunk->instructions[offset + 4];
  // printf("%-16s %d %d %d\n", name, name_constant, def_id, offset_val);
  printf("%-16s %d ", name, name_constant);
  Value constant = chunk->constants[name_constant];
  value_print(constant);
  printf(" %d %d\n", def_id, offset_val);
  return offset + 5;
}

void disassemble_chunk(Chunk *chunk, const char *name) {
  printf("== %s ==\n", name);

  int count = array_count(chunk->instructions);
  for (int offset = 0; offset < count;) {
    offset = disassemble_chunk_instruction(chunk, offset);
  }
}

int disassemble_chunk_instruction(Chunk *chunk, int offset) {
  printf("%04d ", offset);
  if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
    printf("   | ");
  } else {
    printf("%4d ", chunk->lines[offset]);
  }

  uint8_t instruction = chunk->instructions[offset];

  switch (instruction) {
  case OP_CONSTANT:
    return constant_instruction("OP_CONSTANT", chunk, offset);

  case OP_ADD:
    return simple_instruction("OP_ADD", offset);
  case OP_SUBTRACT:
    return simple_instruction("OP_SUBTRACT", offset);
  case OP_MULTIPLY:
    return simple_instruction("OP_MULTIPLY", offset);
  case OP_DIVIDE:
    return simple_instruction("OP_DIVIDE", offset);
  case OP_NEGATE:
    return simple_instruction("OP_NEGATE", offset);

  case OP_EQUAL:
    return simple_instruction("OP_EQUAL", offset);
  case OP_GREATER:
    return simple_instruction("OP_GREATER", offset);
  case OP_LESS:
    return simple_instruction("OP_LESS", offset);
  case OP_NOT:
    return simple_instruction("OP_NOT", offset);

  case OP_NIL:
    return simple_instruction("OP_NIL", offset);
  case OP_TRUE:
    return simple_instruction("OP_TRUE", offset);
  case OP_FALSE:
    return simple_instruction("OP_FALSE", offset);

  case OP_POP:
    return simple_instruction("OP_POP", offset);
  case OP_DEFINE_GLOBAL:
    return constant_instruction("OP_DEFINE_GLOBAL", chunk, offset);
  case OP_GET_GLOBAL:
    return constant_instruction("OP_GET_GLOBAL", chunk, offset);
  case OP_SET_GLOBAL:
    return constant_instruction("OP_SET_GLOBAL", chunk, offset);
  case OP_GET_LOCAL:
    return byte_instruction("OP_GET_LOCAL", chunk, offset);
  case OP_SET_LOCAL:
    return byte_instruction("OP_SET_LOCAL", chunk, offset);
  case OP_GET_UPVALUE:
    return byte_instruction("OP_GET_UPVALUE", chunk, offset);
  case OP_SET_UPVALUE:
    return byte_instruction("OP_SET_UPVALUE", chunk, offset);
  case OP_GET_FIELD:
    return field_instruction("OP_GET_FIELD", chunk, offset);
  case OP_SET_FIELD:
    return field_instruction("OP_SET_FIELD", chunk, offset);

  case OP_JUMP_IF_FALSE:
    return jump_instruction("OP_JUMP_IF_FALSE", chunk, true, offset);
  case OP_JUMP:
    return jump_instruction("OP_JUMP", chunk, true, offset);
  case OP_LOOP:
    return jump_instruction("OP_LOOP", chunk, false, offset);

  case OP_CALL:
    return byte_instruction("OP_CALL", chunk, offset);
  case OP_RETURN:
    return simple_instruction("OP_RETURN", offset);

  case OP_CLOSURE: {
    offset++;
    uint8_t constant_index = chunk->instructions[offset++];
    printf("%-16s %d '", "OP_CLOSURE", constant_index);
    value_print(chunk->constants[constant_index]);
    printf("'\n");

    ObjectFunction *function = AS_FUNCTION(chunk->constants[constant_index]);
    for (int j = 0; j < function->upvalue_count; j++) {
      int is_local = chunk->instructions[offset++];
      int index = chunk->instructions[offset++];
      printf("%04d      |                   %s %d\n", offset - 2,
             is_local ? "local" : "upvalue", index);
    }

    return offset;
  }
  case OP_CLOSE_UPVALUE:
    return simple_instruction("OP_CLOSE_UPVALUE", offset);

  case OP_STRUCT:
    return constant_instruction("OP_STRUCT", chunk, offset);
  case OP_STRUCT_FIELD:
    return constant_instruction("OP_STRUCT_FIELD", chunk, offset);

  default:
    printf("unknown opcode %d\n", instruction);
    return offset + 1;
  }
}
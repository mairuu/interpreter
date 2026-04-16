#pragma once

typedef enum {
  OP_CONSTANT,

  OP_ADD,
  OP_SUBTRACT,
  OP_MULTIPLY,
  OP_DIVIDE,
  OP_NEGATE,

  OP_EQUAL,
  OP_GREATER,
  OP_LESS,
  OP_NOT,

  OP_NIL,
  OP_TRUE,
  OP_FALSE,

  OP_POP,
  OP_DEFINE_GLOBAL,
  OP_GET_GLOBAL,
  OP_SET_GLOBAL,
  OP_GET_LOCAL,
  OP_SET_LOCAL,
  OP_GET_UPVALUE,
  OP_SET_UPVALUE,
  OP_GET_FIELD,
  OP_SET_FIELD,

  OP_JUMP_IF_FALSE,
  OP_JUMP,
  OP_LOOP,

  OP_CLOSURE,
  OP_CLOSE_UPVALUE,

  OP_STRUCT,
  OP_STRUCT_FIELD,

  OP_TRAIT,        // name_const > pushes ObjectTraitDefinition
  OP_TRAIT_METHOD, // name_const > registers method slot on top-of-stack trait

  OP_CALL,
  OP_RETURN,

  // OP_PRINT,
} OpCode;
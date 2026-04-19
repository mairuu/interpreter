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
  OP_POP_SECOND,
  OP_MATCH,

  OP_DEFINE_GLOBAL,
  OP_GET_GLOBAL,
  OP_SET_GLOBAL,
  OP_GET_LOCAL,
  OP_SET_LOCAL,
  OP_GET_UPVALUE,
  OP_SET_UPVALUE,
  OP_GET_PROPERTY,
  OP_SET_PROPERTY,

  OP_JUMP_IF_FALSE,
  OP_JUMP,
  OP_LOOP,

  OP_CLOSURE,
  OP_CONSTRAINT, // slot_idx > binds type constraint to parameter at
                 // slot_idx
  OP_CLOSE_UPVALUE,

  OP_IMPL,        // pops trait_def + struct_def → pushes empty ObjImpl
  OP_IMPL_METHOD, // name_const, pops closure → fills slot on ObjImpl
  OP_IMPL_COMMIT, // pops ObjImpl, registers it

  OP_CAST_TRAIT,
  OP_CALL_METHOD,

  OP_CALL,
  OP_RETURN,

  // OP_PRINT,
} OpCode;
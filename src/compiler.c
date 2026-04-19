#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "compiler.h"
#include "definition_table.h"
#include "dynamic_array.h"
#include "memory.h"
#include "opcode.h"
#include "scanner.h"
#include "string_utils.h"

#define MAX_ASSIGNMENTS                                                        \
  8 // maximum number of variables in a multiple assignment statement, e.g. `var
    // a, b, c = 1, 2, 3`
#define UINT8_COUNT UINT8_MAX + 1

static StringView sv_from_token(Token *token) {
  return sv_create(token->start, token->length);
}

void proto_init(Proto *proto, ProtoType type, Allocator *al) {
  proto->type = type;
  proto->name = str_empty();
  proto->arity = 0;
  proto->upvalue_count = 0;
  proto->code = array_new(al, uint8_t);
  proto->constants = array_new(al, RawConstant);
  proto->lines = array_new(al, int);
}

static void constant_destory(RawConstant constant, Allocator *al) {
  switch (constant.type) {
  case RAW_STRING:
    if (constant.as.string.chars != NULL) {
      al_free(al, constant.as.string.chars, constant.as.string.capacity + 1);
    }
    break;
  case RAW_NIL:
    break;
  case RAW_BOOL:
    break;
  case RAW_NUMBER:
    break;
  case RAW_FUNC:
    break;
  case RAW_STRUCT_DEF: {
    int field_count = array_count(constant.as.struct_def.fields);
    for (int i = 0; i < field_count; i++) {
      str_destroy(&constant.as.struct_def.fields[i], al);
    }
    array_free(constant.as.struct_def.fields, al);
    str_destroy(&constant.as.struct_def.name, al);
    break;
  }
  case RAW_TRAIT_DEF: {
    int method_count = array_count(constant.as.trait_def.methods);
    for (int i = 0; i < method_count; i++) {
      str_destroy(&constant.as.trait_def.methods[i], al);
    }
    str_destroy(&constant.as.trait_def.name, al);
    array_free(constant.as.trait_def.methods, al);
    break;
  }
  case RAW_VARIANT_DEF:
    int arm_count = array_count(constant.as.variant_def.arms);
    for (int i = 0; i < arm_count; i++) {
      str_destroy(&constant.as.variant_def.arms[i].name, al);
      int field_count = array_count(constant.as.variant_def.arms[i].fields);
      for (int j = 0; j < field_count; j++) {
        str_destroy(&constant.as.variant_def.arms[i].fields[j], al);
      }
      array_free(constant.as.variant_def.arms[i].fields, al);
    }
    str_destroy(&constant.as.variant_def.name, al);
    array_free(constant.as.variant_def.arms, al);
    break;
  }
}

void proto_destroy(Proto *proto, Allocator *al) {
  int constant_count = array_count(proto->constants);
  for (int i = 0; i < constant_count; i++) {
    RawConstant constant = proto->constants[i];
    constant_destory(constant, al);
  }
  str_destroy(&proto->name, al);
  array_free(proto->code, al);
  array_free(proto->constants, al);
  array_free(proto->lines, al);
}

void proto_write_byte(Proto *proto, uint8_t byte, int line, Allocator *al) {
  array_push(proto->code, byte, al);
  array_push(proto->lines, line, al);
}

int proto_write_constant(Proto *proto, RawConstant constant, Allocator *al) {
  array_push(proto->constants, constant, al);
  return array_count(proto->constants) - 1;
}

typedef struct {
  Scanner scanner;

  Token current;
  Token previous;

  bool had_error; // set to true if an error has been encountered during
                  // parsing, used to prevent cascading errors

  bool in_panic_mode; // set to true when an error is encountered, used to
                      // suppress error messages until the parser has
                      // synchronized to a known state
} Parser;

static void parser_init(Parser *p, const char *source) {
  scanner_init(&p->scanner, source);
  p->had_error = false;
  p->in_panic_mode = false;
}

typedef struct {
  Token name;
  int depth; // -1 => declared but not defined
  //  0 => global
  // >0 => local
  bool is_captured;
} Local;

typedef struct {
  uint8_t index;
  bool is_local;
} Upvalue;

typedef struct Compiler {
  struct Compiler *enclosing;

  int scope_depth;
  int local_count;
  Local locals[UINT8_COUNT];
  Upvalue upvalues[UINT8_COUNT];

  Proto *proto;
} Compiler;

static void compiler_init(Compiler *compiler, Proto *proto) {
  compiler->enclosing = NULL;
  compiler->scope_depth = 0;
  compiler->local_count = 0;
  compiler->proto = proto;

  Local *local = &compiler->locals[compiler->local_count++];
  local->depth = 0;
  local->name.start = "";
  local->name.length = 0;
  local->is_captured = false;
}

#define MAX_BREAK_JUMPS 32

typedef struct Loop {
  struct Loop *enclosing;
  int start_offset;
  int break_jumps[MAX_BREAK_JUMPS];
  int break_count;
  int scope_depth;
  Token label;
} Loop;

typedef struct {
  const char *source;
  Parser parser;

  Loop *current_loop;
  Compiler *current_compiler;
  DefinitionTable definition;

  Allocator *al;
} CompilerContext;

static void ctx_init(CompilerContext *ctx, const char *source, Allocator *al) {
  ctx->source = source;
  parser_init(&ctx->parser, source);
  ctx->current_loop = NULL;
  ctx->current_compiler = NULL;
  ctx->al = al;

  deftable_init(&ctx->definition, al);
}

static void ctx_destroy(CompilerContext *ctx) {
  deftable_destroy(&ctx->definition, ctx->al);
}

// for speculative parsing
// typedef struct {
//   Parser parser;
//   size_t constants_count;
// } Checkpoint;

// static Checkpoint ctx_checkpoint(CompilerContext *ctx) {
//   return (Checkpoint){.parser = ctx->parser,
//                       .constants_count =
//                           array_count(ctx->current_compiler->proto->constants)};
// }

// static void ctx_restore(CompilerContext *ctx, Checkpoint cp) {
//   ctx->parser = cp.parser;
//   while (array_count(ctx->current_compiler->proto.constants) >
//          cp.constants_count) {
//     RawConstant constant = array_pop(ctx->current_compiler->proto.constants);
//     constant_destory(constant, ctx->al);
//   }
// }

static void ctx_begin_compile(CompilerContext *ctx, Compiler *compiler,
                              ProtoType type) {
  Proto *proto = al_alloc_for(ctx->al, Proto);
  proto_init(proto, type, ctx->al);
  compiler_init(compiler, proto);

  compiler->enclosing = ctx->current_compiler;
  ctx->current_compiler = compiler;

  if (type != PROTO_SCRIPT && ctx->parser.previous.type == TOKEN_IDENTIFIER) {
    proto->name = str_from_str(ctx->parser.previous.start,
                               ctx->parser.previous.length, ctx->al);
  }
}

typedef struct {
  Token token;
  const char *label;
} ErrorLabel;

typedef struct {
  const char *message;
  ErrorLabel labels[4];
  int label_count;
} Diagnostic;

static void ctx_emit_diagnostic(CompilerContext *ctx, Diagnostic *diag) {
  if (ctx->parser.in_panic_mode)
    return;
  ctx->parser.in_panic_mode = true;
  ctx->parser.had_error = true;

  fprintf(stderr, "error: %s\n", diag->message);

  for (int i = 0; i < diag->label_count; i++) {
    ErrorLabel *lb = &diag->labels[i];
    Token *token = &lb->token;

    const char *line_start = token->source;
    while (line_start > ctx->source && line_start[-1] != '\n')
      line_start--;

    const char *line_end = token->source;
    while (*line_end != '\n' && *line_end != '\0')
      line_end++;

    fprintf(stderr, "     |\n");
    fprintf(stderr, "%4d | %.*s\n", token->line_number,
            (int)(line_end - line_start), line_start);
    fprintf(stderr, "     |%*c^ %s\n", (int)(token->source - line_start) + 1,
            ' ', lb->label);
  }
  fprintf(stderr, "\n");
}

#define DIAGNOSTIC(ctx, msg, ...)                                              \
  do {                                                                         \
    ErrorLabel _labels[] = {__VA_ARGS__};                                      \
    Diagnostic _diag = {                                                       \
        .message = (msg),                                                      \
        .label_count = sizeof(_labels) / sizeof(_labels[0]),                   \
        .labels = {},                                                          \
    };                                                                         \
    memcpy(_diag.labels, _labels, sizeof(_labels));                            \
    ctx_emit_diagnostic((ctx), &_diag);                                        \
  } while (0)

static void ctx_error_at(CompilerContext *ctx, Token *token,
                         const char *err_msg) {
  DIAGNOSTIC(ctx, err_msg, {*token, ""});
}

static void ctx_advance(CompilerContext *ctx) {
  ctx->parser.previous = ctx->parser.current;

  while (true) {
    ctx->parser.current = scanner_next_token(&ctx->parser.scanner);
    if (ctx->parser.current.type != TOKEN_ERROR) {
      break;
    }

    printf("error: %.*s\n", ctx->parser.current.length,
           ctx->parser.current.start);
  }
}

// check if the current token is of the given type
static bool ctx_check(CompilerContext *ctx, TokenType type) {
  return ctx->parser.current.type == type;
}

static bool ctx_check_next(CompilerContext *ctx, TokenType type) {
  return scanner_peek_next_token(&ctx->parser.scanner).type == type;
}

// if the current token is of the given type, consume it and return true,
// otherwise return false
static bool ctx_match(CompilerContext *ctx, TokenType type) {
  if (!ctx_check(ctx, type)) {
    return false;
  }

  ctx_advance(ctx);
  return true;
}

static Token ctx_consume_identifier(CompilerContext *ctx, const char *err_msg) {
  if (ctx_check(ctx, TOKEN_IDENTIFIER)) {
    ctx_advance(ctx);
    return ctx->parser.previous;
  }

  ctx_error_at(ctx, &ctx->parser.current, err_msg);
  return (Token){0};
}

// expect the current token to be of the given type and consume it, otherwise
// report an error with the given message
static bool ctx_consume(CompilerContext *ctx, TokenType type,
                        const char *err_msg) {
  if (ctx_check(ctx, type)) {
    ctx_advance(ctx);
    return true;
  }

  ctx_error_at(ctx, &ctx->parser.current, err_msg);

  return false;
}

static void ctx_emit_byte(CompilerContext *ctx, uint8_t byte) {
  proto_write_byte(ctx->current_compiler->proto, byte,
                   ctx->parser.previous.line_number, ctx->al);
}

#define ctx_emit_bytes(ctx, ...)                                               \
  do {                                                                         \
    uint8_t codes[] = {__VA_ARGS__};                                           \
    _ctx_emit_bytes(ctx, codes, sizeof(codes) / sizeof(codes[0]));             \
  } while (0)

static void _ctx_emit_bytes(CompilerContext *ctx, uint8_t *bytes, int length) {
  for (int i = 0; i < length; i++) {
    ctx_emit_byte(ctx, bytes[i]);
  }
}

typedef struct {
  int arg;
  uint8_t op_get;
  uint8_t op_set;
} VarRef;

static void ctx_emit_get(CompilerContext *ctx, VarRef var_ref) {
  ctx_emit_bytes(ctx, var_ref.op_get, var_ref.arg);
}

static void ctx_emit_set(CompilerContext *ctx, VarRef var_ref) {
  ctx_emit_bytes(ctx, var_ref.op_set, var_ref.arg);
}

static uint8_t ctx_make_constant(CompilerContext *ctx, RawConstant value) {
  int constant_index =
      proto_write_constant(ctx->current_compiler->proto, value, ctx->al);

  if (constant_index < UINT8_COUNT) {
    return (uint8_t)constant_index;
  }

  // todo: support more than 256 constants
  ctx_error_at(ctx, &ctx->parser.current, "too many constants in one chunk.");
  return 0;
}

static int ctx_add_raw_constant_struct(CompilerContext *ctx, RawStructDef def) {
  return ctx_make_constant(
      ctx, (RawConstant){.type = RAW_STRUCT_DEF, .as.struct_def = def});
}

static int ctx_add_raw_constant_trait(CompilerContext *ctx, RawTraitDef def) {
  return ctx_make_constant(
      ctx, (RawConstant){.type = RAW_TRAIT_DEF, .as.trait_def = def});
}

static int ctx_add_raw_constant_variant(CompilerContext *ctx,
                                        RawVariantDef def) {
  return ctx_make_constant(
      ctx, (RawConstant){.type = RAW_VARIANT_DEF, .as.variant_def = def});
}

static void ctx_register_definition(CompilerContext *ctx, Token name,
                                    DefinitionKind kind, int constant_index) {
  StringView name_str = sv_from_token(&name);

  DefinitionEntry *existing = deftable_get(&ctx->definition, name_str);
  if (existing != NULL) {
    DIAGNOSTIC(
        ctx, "definition already exists",
        {.token = existing->name_token, .label = "previous definition is here"},
        {.token = name, .label = "conflicting definition is here"});
    return;
  }

  DefinitionEntry entry = {
      .name = str_from_str(name.start, name.length, ctx->al),
      .kind = kind,
      .constant_index = constant_index,
      .name_token = name,
  };

  if (!deftable_put(&ctx->definition, entry, ctx->al)) {
    ctx_error_at(ctx, &name, "failed to add definition to table.");
  }
}

static void ctx_emit_constant(CompilerContext *ctx, RawConstant value) {
  ctx_emit_bytes(ctx, OP_CONSTANT, ctx_make_constant(ctx, value));
}

static void ctx_emit_return(CompilerContext *ctx) {
  if (array_count(ctx->current_compiler->proto->code) > 0 &&
      array_peek(ctx->current_compiler->proto->code) == OP_RETURN) {
    return;
  }

  ctx_emit_byte(ctx, OP_NIL);
  ctx_emit_byte(ctx, OP_RETURN);
}

static void ctx_emit_token_literal(CompilerContext *ctx, Token *literal) {
  if (literal->type == TOKEN_NIL) {
    ctx_emit_byte(ctx, OP_NIL);
  } else if (literal->type == TOKEN_TRUE) {
    ctx_emit_byte(ctx, OP_TRUE);
  } else if (literal->type == TOKEN_FALSE) {
    ctx_emit_byte(ctx, OP_FALSE);
  } else if (literal->type == TOKEN_NUMBER) {
    ctx_emit_byte(ctx, OP_CONSTANT);
    double num = strtod(literal->start, NULL);
    RawConstant constant = {.type = RAW_NUMBER, .as.number = num};
    ctx_emit_byte(ctx, ctx_make_constant(ctx, constant));
  } else {
    ctx_error_at(ctx, literal, "not a literal token");
  }
}

static Proto *ctx_end_compile(CompilerContext *ctx) {
  ctx_emit_return(ctx);

  Compiler *compiler = ctx->current_compiler;

  ctx->current_compiler = compiler->enclosing;
  return compiler->proto;
}

static void ctx_synchronize(CompilerContext *ctx) {
  ctx->parser.in_panic_mode = false;

  while (ctx->parser.current.type != TOKEN_EOF) {
    if (ctx->parser.current.type == TOKEN_RIGHT_BRACE) {
      ctx_advance(ctx);
      return;
    }

    switch (ctx->parser.current.type) {
    case TOKEN_RIGHT_BRACE:
    // case TOKEN_CLASS:
    case TOKEN_FUN:
    case TOKEN_VAR:
    case TOKEN_FOR:
    case TOKEN_IF:
    // case TOKEN_PRINT:
    case TOKEN_RETURN:
    case TOKEN_STRUCT:
    case TOKEN_TRAIT:
    case TOKEN_IMPL:
    case TOKEN_VARIANT:
      return;
    default:
      break;
    }

    ctx_advance(ctx);
  }
}

typedef void (*ParseFunc)(CompilerContext *ctx, bool can_assign);

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT, // =
  PREC_OR,         // or
  PREC_AND,        // and
  PREC_EQUALITY,   // == !=
  PREC_COMPARISON, // < > <= >=
  PREC_TERM,       // + -
  PREC_FACTOR,     // * /
  PREC_UNARY,      // ! -
  PREC_CALL,       // . ()
  PREC_PRIMARY
} Precedence;

typedef struct {
  ParseFunc prefix;
  ParseFunc infix;
  Precedence precedence;
} ParseRule;

static void grouping(CompilerContext *ctx, bool can_assign);
static void call(CompilerContext *ctx, bool can_assign);
static void number(CompilerContext *ctx, bool can_assign);
static void unary(CompilerContext *ctx, bool can_assign);
static void binary(CompilerContext *ctx, bool can_assign);
static void literal(CompilerContext *ctx, bool can_assign);
static void string(CompilerContext *ctx, bool can_assign);
static void variable(CompilerContext *ctx, bool can_assign);
static void and_(CompilerContext *ctx, bool can_assign);
static void or_(CompilerContext *ctx, bool can_assign);
static void dot(CompilerContext *ctx, bool can_assign);
static void as(CompilerContext *ctx, bool can_assign);

// expression
static void if_(CompilerContext *ctx, bool can_assign);
static void fun(CompilerContext *ctx, bool can_assign);
static void match(CompilerContext *ctx, bool can_assign);

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN] = {grouping, call, PREC_CALL},
    [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},
    [TOKEN_LEFT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_COLON] = {NULL, NULL, PREC_NONE},
    [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
    [TOKEN_DOT] = {NULL, dot, PREC_CALL},
    [TOKEN_MINUS] = {unary, binary, PREC_TERM},
    [TOKEN_MINUS_MINUS] = {NULL, NULL, PREC_NONE},
    [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
    [TOKEN_PLUS_PLUS] = {NULL, NULL, PREC_NONE},
    [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
    [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
    [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},

    [TOKEN_BANG] = {unary, NULL, PREC_NONE},
    [TOKEN_BANG_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_EQUAL_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_GREATER] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_FAT_ARROW] = {NULL, NULL, PREC_NONE},

    [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
    [TOKEN_STRING] = {string, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},

    [TOKEN_AND] = {NULL, and_, PREC_AND},
    [TOKEN_AS] = {NULL, as, PREC_CALL},
    [TOKEN_BREAK] = {NULL, NULL, PREC_NONE},
    // [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
    [TOKEN_CONTINUE] = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
    [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
    [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
    [TOKEN_FUN] = {fun, NULL, PREC_NONE},
    [TOKEN_IF] = {if_, NULL, PREC_NONE},
    [TOKEN_IS] = {NULL, NULL, PREC_NONE},
    [TOKEN_IMPL] = {NULL, NULL, PREC_NONE},
    [TOKEN_MATCH] = {match, NULL, PREC_NONE},
    [TOKEN_NIL] = {literal, NULL, PREC_NONE},
    [TOKEN_OR] = {NULL, or_, PREC_OR},
    // [TOKEN_PRINT] = {NULL, NULL, PREC_NONE},
    [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},
    // [TOKEN_SUPER] = {NULL, NULL, PREC_NONE},
    [TOKEN_STRUCT] = {NULL, NULL, PREC_NONE},
    [TOKEN_THIS] = {NULL, NULL, PREC_NONE},
    [TOKEN_TRUE] = {literal, NULL, PREC_NONE},
    [TOKEN_VAR] = {NULL, NULL, PREC_NONE},

    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
};

static const ParseRule *get_rule(TokenType type) { return &rules[type]; }

static void parse_precedence(CompilerContext *ctx, Precedence precedence) {
  ctx_advance(ctx);
  ParseFunc prefix_rule = get_rule(ctx->parser.previous.type)->prefix;
  if (prefix_rule == NULL) {
    ctx_error_at(ctx, &ctx->parser.previous, "expect expression.");
    return;
  }

  bool can_assign = precedence <= PREC_ASSIGNMENT;
  prefix_rule(ctx, can_assign);

  while (precedence <= get_rule(ctx->parser.current.type)->precedence) {
    ctx_advance(ctx);
    ParseFunc infix_rule = get_rule(ctx->parser.previous.type)->infix;
    infix_rule(ctx, can_assign);
  }

  if (can_assign && ctx_match(ctx, TOKEN_EQUAL)) {
    ctx_error_at(ctx, &ctx->parser.previous, "invalid assignment target.");
  }
}

static void expression(CompilerContext *ctx) {
  parse_precedence(ctx, PREC_ASSIGNMENT);
}

static bool identifier_equals(Token *a, Token *b);
static uint8_t ctx_identifier_constant(CompilerContext *ctx, Token *name);

static int ctx_resolve_local(CompilerContext *ctx, Compiler *compiler,
                             Token *name) {
  for (int i = compiler->local_count - 1; i >= 0; i--) {
    Local *local = &compiler->locals[i];
    if (!identifier_equals(name, &local->name)) {
      continue; // keep looking
    }

    if (local->depth == -1) {
      ctx_error_at(ctx, name,
                   "can't read local variable in its own initializer.");
    }

    return i;
  }

  return -1;
}

static int ctx_add_upvalue(CompilerContext *ctx, Compiler *compiler,
                           uint8_t index, bool is_local) {
  int upvalue_count = compiler->proto->upvalue_count;

  for (int i = 0; i < upvalue_count; i++) {
    Upvalue *upvalue = &compiler->upvalues[i];
    if (upvalue->index == index && upvalue->is_local == is_local) {
      return i;
    }
  }

  if (upvalue_count >= UINT8_COUNT) {
    ctx_error_at(ctx, &ctx->parser.current,
                 "too many closure variables in function.");
    return 0;
  }

  compiler->upvalues[upvalue_count].is_local = is_local;
  compiler->upvalues[upvalue_count].index = index;
  return compiler->proto->upvalue_count++;
}

static int ctx_resolve_upvalue(CompilerContext *ctx, Compiler *compiler,
                               Token *name) {
  if (compiler->enclosing == NULL) {
    return -1; // not found
  }

  int local_index = ctx_resolve_local(ctx, compiler->enclosing, name);
  if (local_index != -1) {
    compiler->enclosing->locals[local_index].is_captured = true;
    return ctx_add_upvalue(ctx, compiler, (uint8_t)local_index, true);
  }

  int upvalue_index = ctx_resolve_upvalue(ctx, compiler->enclosing, name);
  if (upvalue_index != -1) {
    return ctx_add_upvalue(ctx, compiler, (uint8_t)upvalue_index, false);
  }

  return -1; // not found
}

static VarRef ctx_resolve_variable(CompilerContext *ctx, Token name) {
  VarRef ref;
  Compiler *target = ctx->current_compiler;
  if ((ref.arg = ctx_resolve_local(ctx, target, &name)) > -1) {
    ref.op_get = OP_GET_LOCAL;
    ref.op_set = OP_SET_LOCAL;
  } else if ((ref.arg = ctx_resolve_upvalue(ctx, target, &name)) > -1) {
    ref.op_get = OP_GET_UPVALUE;
    ref.op_set = OP_SET_UPVALUE;
  } else {
    ref.arg = ctx_identifier_constant(ctx, &name);
    ref.op_get = OP_GET_GLOBAL;
    ref.op_set = OP_SET_GLOBAL;
  }
  return ref;
}

static void check_not_definition(CompilerContext *ctx, Token *name) {
  DefinitionEntry *entry = deftable_get(&ctx->definition, sv_from_token(name));
  if (entry != NULL) {
    ctx_error_at(ctx, name, "definition cannot be assigned to a variable.");
  }
}

static void ctx_named_variable(CompilerContext *ctx, Token name,
                               bool can_assign) {
  VarRef ref = ctx_resolve_variable(ctx, name);

  if (can_assign && ctx_match(ctx, TOKEN_EQUAL)) {
    if (ref.op_set == OP_SET_GLOBAL) {
      check_not_definition(ctx, &name);
    }
    expression(ctx);
    ctx_emit_set(ctx, ref);
  } else {
    ctx_emit_get(ctx, ref);
  }
}

static Loop *ctx_resolve_loop(CompilerContext *ctx, Token *label) {
  Loop *current = ctx->current_loop;

  while (current != NULL) {
    if (identifier_equals(&current->label, label)) {
      return current;
    }
    current = current->enclosing;
  }

  return NULL; // label not found
}

// static bool statement_multiple_assignment(CompilerContext *ctx) {
//   // check IDENT [, IDENT]*
//   if (!ctx_check(ctx, TOKEN_IDENTIFIER)) {
//     return false;
//   }

//   Checkpoint saved = ctx_checkpoint(ctx);

//   VarRef refs[MAX_ASSIGNMENTS];
//   Token targets[MAX_ASSIGNMENTS];
//   int count = 0;

//   ctx_advance(ctx);
//   targets[count] = ctx->parser.previous;
//   refs[count] = ctx_resolve_variable(ctx, targets[count]);
//   count++;

//   while (ctx_match(ctx, TOKEN_COMMA)) {
//     if (!ctx_check(ctx, TOKEN_IDENTIFIER)) {
//       ctx_restore(ctx, saved);
//       return false;
//     }

//     if (count >= MAX_ASSIGNMENTS) {
//       ctx_error_at(ctx, &ctx->parser.current,
//                    "too many variables in multiple assignment.");

//       // consume through '=' and skip rhs expressions so parser can continue
//       while (!ctx_check(ctx, TOKEN_EQUAL) && !ctx_check(ctx, TOKEN_EOF))
//         ctx_advance(ctx);

//       if (ctx_match(ctx, TOKEN_EQUAL)) {
//         do {
//           expression(ctx);
//         } while (ctx_match(ctx, TOKEN_COMMA));
//       }
//       return true;
//     }

//     targets[count] = ctx->parser.current;
//     ctx_advance(ctx);
//     refs[count] = ctx_resolve_variable(ctx, targets[count]);
//     count++;
//   }

//   if (!ctx_match(ctx, TOKEN_EQUAL)) {
//     ctx_restore(ctx, saved);
//     return false;
//   }

//   // confirmed multi-assignment; parse rhs
//   int rhs_count = 0;
//   do {
//     expression(ctx);
//     rhs_count++;
//   } while (ctx_match(ctx, TOKEN_COMMA));

//   if (rhs_count == 1 && count > 1) {
//     // unpacking assignment
//     ctx_emit_bytes(ctx, OP_UNPACK, count);
//     for (int i = count - 1; i >= 0; i--) {
//       ctx_emit_set(ctx, refs[i]);
//       ctx_emit_byte(ctx, OP_POP);
//     }
//   } else if (rhs_count == count) {
//     // parallel assignment
//     for (int i = count - 1; i >= 0; i--) {
//       ctx_emit_set(ctx, refs[i]);
//       ctx_emit_byte(ctx, OP_POP);
//     }
//   } else {
//     ctx_error_at(ctx, &ctx->parser.previous,
//                  "number of values does not match number of targets.");
//   }

//   return true;
// }

static void statement_expression(CompilerContext *ctx) {
  // if (statement_multiple_assignment(ctx)) {
  //   return;
  // }

  expression(ctx);
  ctx_emit_byte(ctx, OP_POP);
}

// static void statement_print(CompilerContext *ctx) {
//   expression(ctx);
//   ctx_emit_byte(ctx, OP_PRINT);
// }

static void declaration(CompilerContext *ctx);

static void statement_block(CompilerContext *ctx) {
  while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF)) {
    declaration(ctx);
  }

  ctx_consume(ctx, TOKEN_RIGHT_BRACE, "expect '}' after block.");
}

static void ctx_begin_scope(CompilerContext *ctx) {
  ctx->current_compiler->scope_depth++;
}

static void ctx_end_scope(CompilerContext *ctx) {
  Compiler *c = ctx->current_compiler;
  c->scope_depth--;

  // pop local variables that are out of scope
  while (c->local_count > 0 &&
         c->locals[c->local_count - 1].depth > c->scope_depth) {
    if (c->locals[c->local_count - 1].is_captured) {
      ctx_emit_byte(ctx, OP_CLOSE_UPVALUE);
    } else {
      ctx_emit_byte(ctx, OP_POP);
    }
    c->local_count--;
  }
}

static size_t ctx_code_offset(CompilerContext *ctx) {
  return array_count(ctx->current_compiler->proto->code);
}

static int ctx_emit_jump(CompilerContext *ctx, uint8_t op) {
  ctx_emit_bytes(ctx, op, 0, 0);
  return ctx_code_offset(ctx) - 2;
}

static void ctx_patch_jump(CompilerContext *ctx, int offset) {
  // [offset + -1] OP_JUMP
  // [offset + 0] A, B
  // [offset + 2] ...code (N bytes)
  // [offset + 2 + N] (next instruction)

  // N = {next instruction} - offset - 2
  int jump = ctx_code_offset(ctx) - offset - 2;

  if (jump > UINT16_MAX) {
    ctx_error_at(ctx, &ctx->parser.current, "too much code to jump over.");
  }

  ctx->current_compiler->proto->code[offset + 0] = (jump >> 8) & 0xff; // A
  ctx->current_compiler->proto->code[offset + 1] = (jump >> 0) & 0xff; // B
}

static void ctx_emit_loop(CompilerContext *ctx, int loop_start) {
  ctx_emit_byte(ctx, OP_LOOP);

  int offset = ctx_code_offset(ctx) - loop_start + 2;
  if (offset > UINT16_MAX) {
    ctx_error_at(ctx, &ctx->parser.current, "loop body too large.");
  }

  ctx_emit_bytes(ctx, (offset >> 8) & 0xff, offset & 0xff);
}

static void statement_if(CompilerContext *ctx) {
  expression(ctx);

  int else_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);
  ctx_emit_byte(ctx, OP_POP); // pop condition result

  ctx_begin_scope(ctx);
  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' after 'if' condition.");
  statement_block(ctx);
  ctx_end_scope(ctx);

  int exit_jump = ctx_emit_jump(ctx, OP_JUMP);

  ctx_patch_jump(ctx, else_jump);
  ctx_emit_byte(ctx, OP_POP);

  if (ctx_match(ctx, TOKEN_ELSE)) {
    if (ctx_match(ctx, TOKEN_IF)) {
      statement_if(ctx); // else if
    } else {
      ctx_begin_scope(ctx);
      ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' after 'else'.");
      statement_block(ctx);
      ctx_end_scope(ctx);
    }
  }

  ctx_patch_jump(ctx, exit_jump);
}

static void declaration_var(CompilerContext *ctx);

static void statement_for(CompilerContext *ctx, Token *label) {
  ctx_begin_scope(ctx);

  // int loop_start = array_count(ctx_current_chunk(ctx)->code);
  int loop_start = ctx_code_offset(ctx);
  int exit_jump = -1;
  bool has_condition = false;

  if (ctx_match(ctx, TOKEN_SEMICOLON)) {
    // empty initializer: `for ; ...`
  } else if (ctx_check(ctx, TOKEN_LEFT_BRACE)) {
    // infinite loop: `for {}`
  } else if (ctx_match(ctx, TOKEN_VAR)) {
    // variable declaration: `for var i = 0; ...`
    declaration_var(ctx);
    ctx_match(ctx, TOKEN_SEMICOLON); // optional
    loop_start = ctx_code_offset(ctx);
  } else {
    // ambiguous expression
    int expr_start = ctx_code_offset(ctx);
    expression(ctx);

    if (ctx_match(ctx, TOKEN_SEMICOLON)) {
      // it was an initializer (`for a = 0; ...`)
      ctx_emit_byte(ctx, OP_POP); // discard the initializer's result from stack
      loop_start = array_count(
          ctx->current_compiler->proto->code); // loop actually starts here
    } else {
      // no semicolon means it was the condition (`for a < 3 {}`)
      loop_start = expr_start;
      has_condition = true;
    }
  }

  if (!has_condition && !ctx_check(ctx, TOKEN_LEFT_BRACE)) {
    if (ctx_match(ctx, TOKEN_SEMICOLON)) {
      // explicitly omitted condition: `for ; ; {}`
    } else {
      // parse explicit condition: `for a = 0; a < 3; {}`
      expression(ctx);
      has_condition = true;
      ctx_match(ctx, TOKEN_SEMICOLON); // consume optional trailing semicolon
    }
  }

  if (has_condition) {
    exit_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);
    ctx_emit_byte(ctx, OP_POP);
  }

  // if we did not hit body's left brace, it means we have an increment clause
  if (!ctx_check(ctx, TOKEN_LEFT_BRACE)) {
    int body_jump = ctx_emit_jump(ctx, OP_JUMP);
    int increment_start = ctx_code_offset(ctx);

    expression(ctx);
    ctx_match(ctx, TOKEN_SEMICOLON); // optional
    ctx_emit_byte(ctx, OP_POP);

    ctx_emit_loop(ctx, loop_start);
    loop_start = increment_start;
    ctx_patch_jump(ctx, body_jump);
  }

  Loop loop = {
      .enclosing = ctx->current_loop,
      .start_offset = loop_start,
      .break_count = 0,
      .scope_depth = ctx->current_compiler->scope_depth,
      .label = label != NULL ? *label : (Token){0},
  };
  ctx->current_loop = &loop;

  ctx_match(ctx, TOKEN_SEMICOLON); // optional semicolon before body
  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' before for body.");

  ctx_begin_scope(ctx);
  statement_block(ctx);
  ctx_end_scope(ctx);

  ctx_emit_loop(ctx, loop_start);
  if (exit_jump != -1) {
    ctx_patch_jump(ctx, exit_jump);
    ctx_emit_byte(ctx, OP_POP);
  }

  // patch break jumps to the end of the loop.
  for (int i = 0; i < loop.break_count; i++) {
    ctx_patch_jump(ctx, loop.break_jumps[i]);
  }

  ctx->current_loop = loop.enclosing;

  ctx_end_scope(ctx);
}

static void statement_break(CompilerContext *ctx) {
  if (ctx->current_loop == NULL) {
    ctx_error_at(ctx, &ctx->parser.current,
                 "can't use 'break' outside of a loop.");
    return;
  }

  Loop *target_loop = ctx->current_loop;

  if (ctx_match(ctx, TOKEN_IDENTIFIER)) {
    target_loop = ctx_resolve_loop(ctx, &ctx->parser.previous);
    if (target_loop == NULL) {
      ctx_error_at(ctx, &ctx->parser.previous, "undefined loop label.");
      return;
    }
  }

  // pop local variables until end of loop's scope
  const Compiler *c = ctx->current_compiler;
  for (int i = c->local_count - 1;
       i >= 0 && c->locals[i].depth > target_loop->scope_depth; i--) {
    ctx_emit_byte(ctx, OP_POP);
  }

  if (target_loop->break_count >= MAX_BREAK_JUMPS) {
    ctx_error_at(ctx, &ctx->parser.current,
                 "too many 'break' statements in one loop.");
    return;
  }

  int jump_offset = ctx_emit_jump(ctx, OP_JUMP);
  target_loop->break_jumps[target_loop->break_count++] = jump_offset;
}

static void statement_continue(CompilerContext *ctx) {
  if (ctx->current_loop == NULL) {
    ctx_error_at(ctx, &ctx->parser.current,
                 "can't use 'continue' outside of a loop.");
    return;
  }

  Loop *target_loop = ctx->current_loop;

  if (ctx_match(ctx, TOKEN_IDENTIFIER)) {
    target_loop = ctx_resolve_loop(ctx, &ctx->parser.previous);
    if (target_loop == NULL) {
      ctx_error_at(ctx, &ctx->parser.previous, "undefined loop label.");
      return;
    }
  }

  // pop local variables until end of loop's scope
  const Compiler *c = ctx->current_compiler;
  for (int i = c->local_count - 1;
       i >= 0 && c->locals[i].depth > target_loop->scope_depth; i--) {
    ctx_emit_byte(ctx, OP_POP);
  }

  ctx_emit_loop(ctx, target_loop->start_offset);
}

static void statement_label(CompilerContext *ctx) {
  Token label = ctx->parser.current;
  ctx_advance(ctx); // identifier
  ctx_advance(ctx); // colon

  if (ctx_match(ctx, TOKEN_FOR)) {
    statement_for(ctx, &label);
  } else {
    ctx_error_at(ctx, &label,
                 "unexpected statement label."
                 "\n labels can only be applied to loops");
  }
}

static void statement_return(CompilerContext *ctx) {
  if (ctx->current_compiler->proto->type == PROTO_SCRIPT) {
    ctx_error_at(ctx, &ctx->parser.previous,
                 "can't return from top-level code.");
  }

  if (ctx_check(ctx, TOKEN_RIGHT_BRACE)) {
    ctx_emit_return(ctx);
  } else {
    expression(ctx);
    ctx_emit_byte(ctx, OP_RETURN);
  }
}

typedef enum {
  ARM_PATTERN_LITERAL,
  ARM_PATTERN_WILDCARD, // _
} ArmPatternKind;

typedef struct {
  ArmPatternKind kind;
  union {
    Token literal_token; // for ARM_PATTERN_LITERAL
  };
} ArmPattern;

static bool token_is_underscore(Token *token) {
  return token->type == TOKEN_IDENTIFIER && token->length == 1 && token->start[0] == '_';
}

ArmPattern ctx_parse_arm_pattern(CompilerContext *ctx) {
  // {literal}
  ArmPattern pattern = {0};

  if (ctx_check(ctx, TOKEN_NUMBER) || ctx_check(ctx, TOKEN_STRING) ||
      ctx_check(ctx, TOKEN_TRUE) || ctx_check(ctx, TOKEN_FALSE) ||
      ctx_check(ctx, TOKEN_NIL)) {
    pattern.kind = ARM_PATTERN_LITERAL;
    pattern.literal_token = ctx->parser.current;
    ctx_advance(ctx);
  // } else if (ctx_match(ctx, TOKEN_UNDERSCORE)) {
  } else if (token_is_underscore(&ctx->parser.current)) {
    ctx_advance(ctx);
    pattern.kind = ARM_PATTERN_WILDCARD;
  } else {
    ctx_error_at(ctx, &ctx->parser.current, "unsupported match arm pattern.");
  }

  return pattern;
}

typedef enum {
  MATCH_MODE_STATEMENT,
  MATCH_MODE_EXPRESSION,
} MatchMode;

static void ctx_compile_match_arm_body(CompilerContext *ctx, MatchMode mode) {
  if (mode == MATCH_MODE_EXPRESSION) {
    expression(ctx);
  } else {
    if (!ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' before match arm body.")) {
      // synchronize to next arm or end of match
      expression(ctx);
      return;
    }
    ctx_begin_scope(ctx);
    statement_block(ctx);
    ctx_end_scope(ctx);
  }
}

static void ctx_compile_match_arm(CompilerContext *ctx, MatchMode mode,
                                  int *end_jumps) {
  // <arm_pattern> => <arm_body>
  ArmPattern pattern = ctx_parse_arm_pattern(ctx);

  switch (pattern.kind) {
  case ARM_PATTERN_LITERAL:
    ctx_emit_token_literal(ctx, &pattern.literal_token);
    ctx_emit_byte(ctx, OP_MATCH); // [a, b] => [a, (a == b)]
    break;
  case ARM_PATTERN_WILDCARD:
    ctx_emit_constant(ctx, RAW_BOOL_VALUE(true)); // [a, _] => [a, true]
    break;
  default:
    assert(false && "unhandled arm pattern kind");
    break;
  }

  int next_arm_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);
  ctx_emit_byte(ctx, OP_POP);

  // ArmBinding binding = ctx_compile_pattern_bindings(ctx, &pattern);

  ctx_consume(ctx, TOKEN_FAT_ARROW, "expect '=>' after match arm pattern.");

  ctx_compile_match_arm_body(ctx, mode);
  // ctx_compile_match_arm_body(ctx, mode, binding);

  array_push(end_jumps, ctx_emit_jump(ctx, OP_JUMP), ctx->al);
  ctx_patch_jump(ctx, next_arm_jump);
  ctx_emit_byte(ctx, OP_POP);
}

// mode == expression => match <expr> { <arm_pattern> => <expr> }
// mode == statement  => match <expr> { <arm_pattern> => <block> }
static void ctx_compile_match(CompilerContext *ctx, MatchMode mode) {
  // match <expr> { ... }
  expression(ctx);

  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' after 'match' expression.");

  int *end_jumps;
  array_init(end_jumps, int, ctx->al);

  while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF)) {
    ctx_compile_match_arm(ctx, mode, end_jumps);
  }

  ctx_consume(ctx, TOKEN_RIGHT_BRACE, "expect '}' after 'match' body.");

  int end_jump_count = array_count(end_jumps);
  for (int i = 0; i < end_jump_count; i++) {
    ctx_patch_jump(ctx, end_jumps[i]);
  }
  array_free(end_jumps, ctx->al);

  if (mode == MATCH_MODE_EXPRESSION) {
    ctx_emit_byte(ctx, OP_POP_SECOND);
  } else {
    ctx_emit_bytes(ctx, OP_POP);
  }
}

static void statement_match(CompilerContext *ctx) {
  ctx_compile_match(ctx, MATCH_MODE_STATEMENT);
}

static void statement(CompilerContext *ctx) {
  // if (ctx_match(ctx, TOKEN_PRINT)) {
  //   statement_print(ctx);
  // }

  if (ctx_match(ctx, TOKEN_IF)) {
    statement_if(ctx);
  } else if (ctx_match(ctx, TOKEN_FOR)) {
    statement_for(ctx, NULL);
  } else if (ctx_match(ctx, TOKEN_BREAK)) {
    statement_break(ctx);
  } else if (ctx_match(ctx, TOKEN_CONTINUE)) {
    statement_continue(ctx);
  } else if (ctx_match(ctx, TOKEN_LEFT_BRACE)) {
    ctx_begin_scope(ctx);
    statement_block(ctx);
    ctx_end_scope(ctx);
  } else if (ctx_check(ctx, TOKEN_IDENTIFIER) &&
             ctx_check_next(ctx, TOKEN_COLON)) {
    statement_label(ctx);
  } else if (ctx_match(ctx, TOKEN_RETURN)) {
    statement_return(ctx);
  } else if (ctx_match(ctx, TOKEN_MATCH)) {
    statement_match(ctx);
  } else {
    statement_expression(ctx);
  }
}

static uint8_t ctx_identifier_constant(CompilerContext *ctx, Token *name) {
  char *name_str = copy_string(name->start, name->length, ctx->al);
  return ctx_make_constant(
      ctx, RAW_STRING_VALUE(name_str, name->length, name->length));
}

static bool identifier_equals(Token *a, Token *b) {
  return a->length == b->length && memcmp(a->start, b->start, a->length) == 0;
}

static void ctx_add_local(CompilerContext *ctx, Token *name) {
  if (ctx->current_compiler->local_count >= UINT8_COUNT) {
    ctx_error_at(ctx, name, "too many local variables in function.");
    return;
  }

  Local *local =
      &ctx->current_compiler->locals[ctx->current_compiler->local_count++];

  local->name = *name;
  local->depth = -1;
}

static void ctx_declare_variable(CompilerContext *ctx) {
  if (ctx->current_compiler->scope_depth == 0) {
    return;
  }

  Token *name = &ctx->parser.previous;

  for (int i = ctx->current_compiler->local_count - 1; i >= 0; i--) {
    Local *local = &ctx->current_compiler->locals[i];
    if (local->depth != -1 &&
        local->depth < ctx->current_compiler->scope_depth) {
      break; // out of current depth
    }

    if (identifier_equals(name, &local->name)) {
      DIAGNOSTIC(ctx, "already a variable with this name in this scope.",
                 {local->name, "first declared here"},
                 {ctx->parser.previous, "re-declared here"});
    }
  }

  ctx_add_local(ctx, name);
}

static uint8_t ctx_parse_variable(CompilerContext *ctx, const char *err_msg) {
  ctx_consume(ctx, TOKEN_IDENTIFIER, err_msg);
  ctx_declare_variable(ctx);

  // local
  if (ctx->current_compiler->scope_depth > 0) {
    return 0;
  }

  // global
  return ctx_identifier_constant(ctx, &ctx->parser.previous);
}

// mark all variables in the current scope as initialized
static void ctx_mark_initialized(CompilerContext *ctx) {
  if (ctx->current_compiler->scope_depth == 0) {
    return;
  }

  Compiler *c = ctx->current_compiler;
  for (int i = c->local_count - 1; i >= 0; i--) {
    Local *local = &c->locals[i];
    if (local->depth != -1 &&
        local->depth < ctx->current_compiler->scope_depth) {
      break; // out of current depth
    }

    if (local->depth == -1) {
      local->depth = ctx->current_compiler->scope_depth;
    }
  }
}

static void ctx_define_variable(CompilerContext *ctx, uint8_t global_index) {
  // local
  if (ctx->current_compiler->scope_depth > 0) {
    ctx_mark_initialized(ctx);
    return;
  }

  // global
  ctx_emit_bytes(ctx, OP_DEFINE_GLOBAL, global_index);
}

static void declaration_var(CompilerContext *ctx) {
  uint8_t index = ctx_parse_variable(ctx, "expect variable name");
  Token name = ctx->parser.previous;

  if (ctx->current_compiler->enclosing == NULL) {
    DefinitionEntry *existing =
        deftable_get(&ctx->definition, sv_from_token(&name));
    if (existing != NULL) {
      char buf[256];
      snprintf(
          buf, sizeof(buf),
          "'%.*s' is a %s definition and cannot be shadowed by a variable.",
          name.length, name.start, definition_kind_name(existing->kind));
      DIAGNOSTIC(ctx, buf, {existing->name_token, "definition declared here"},
                 {name, "variable declared here"}, );
      ctx_advance(ctx); // skip initializer expression
      return;
    }
  }

  if (ctx_match(ctx, TOKEN_EQUAL)) {
    expression(ctx);
  } else {
    ctx_emit_byte(ctx, OP_NIL);
  }

  ctx_define_variable(ctx, index);
}

static int ctx_parse_parameter_list(CompilerContext *ctx, ProtoType type,
                                    Token *type_constraint) {
  ctx_consume(ctx, TOKEN_LEFT_PAREN, "expect '(' after function name");

  int param_count = 0;
  Token self = token_from_cstr(TOKEN_IDENTIFIER, "self");

  if (!ctx_check(ctx, TOKEN_RIGHT_PAREN)) {
    do {
      param_count++;
      if (param_count >= UINT8_COUNT) {
        ctx_error_at(ctx, &ctx->parser.previous,
                     "can't have more than 255 parameters.");
      }

      Token name = ctx->parser.current;

      if (type == PROTO_METHOD && param_count == 1) {
        ctx_consume(ctx, TOKEN_IDENTIFIER,
                    "expect 'self' parameter for a method.");

        if (!identifier_equals(&ctx->parser.previous, &self)) {
          ctx_error_at(ctx, &ctx->parser.previous,
                       "first parameter of a method must be named 'self'.");
        }

        if (ctx_check(ctx, TOKEN_COLON)) {
          ctx_advance(ctx);
          ctx_error_at(ctx, &ctx->parser.previous,
                       "self parameter cannot have a constraint.");
          ctx_advance(ctx); // constraint identifier
        }
      } else {
        uint8_t param_index = ctx_parse_variable(ctx, "expect parameter name");
        ctx_define_variable(ctx, param_index);

        if (ctx_match(ctx, TOKEN_COLON)) {
          ctx_consume(ctx, TOKEN_IDENTIFIER,
                      "expect type constraint after ':'.");
          Token param_name = ctx->parser.previous;
          type_constraint[param_count - 1] = param_name;
        } else {
          type_constraint[param_count - 1] = (Token){0};
        }
      }

      DefinitionEntry *existing =
          deftable_get(&ctx->definition, sv_from_token(&name));
      if (existing != NULL) {
        char buf[256];
        snprintf(
            buf, sizeof(buf),
            "'%.*s' is a %s definition and cannot be shadowed by a parameter.",
            name.length, name.start, definition_kind_name(existing->kind));
        DIAGNOSTIC(ctx, buf, {existing->name_token, "definition declared here"},
                   {name, "parameter declared here"}, );
      }
    } while (ctx_match(ctx, TOKEN_COMMA));
  }

  if (type == PROTO_METHOD) {
    if (param_count == 0) {
      ctx_error_at(ctx, &ctx->parser.current,
                   "expect 'self' parameter for a method.");
    }
    param_count--;
  }

  ctx_consume(ctx, TOKEN_RIGHT_PAREN, "expect ')' after function parameters.");
  return param_count;
}

// parse arguments and body of a function
static uint8_t ctx_compile_function(CompilerContext *ctx, ProtoType type,
                                    Token *name) {
  Compiler compiler;
  ctx_begin_compile(ctx, &compiler, type);

  if (type == PROTO_METHOD) {
    compiler.locals[0].name = token_from_cstr(TOKEN_IDENTIFIER, "self");
  } else if (name != NULL) {
    compiler.locals[0].name = *name;
  }

  ctx_begin_scope(ctx);

  Token type_constraint[10] = {0};
  compiler.proto->arity = ctx_parse_parameter_list(ctx, type, type_constraint);

  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' before function body.");
  statement_block(ctx);

  Proto *proto = ctx_end_compile(ctx);
  uint8_t proto_constant = ctx_make_constant(ctx, RAW_FUNC_VALUE(proto));

  if (compiler.proto->upvalue_count > 0) {
    ctx_emit_bytes(ctx, OP_CLOSURE, proto_constant);
    for (int i = 0; i < compiler.proto->upvalue_count; i++) {
      Upvalue *upvalue = &compiler.upvalues[i];
      ctx_emit_byte(ctx, upvalue->is_local ? 1 : 0);
      ctx_emit_byte(ctx, upvalue->index);
    }
  } else {
    ctx_emit_bytes(ctx, OP_CONSTANT, proto_constant);
  }

  // int scope_depth = ctx->current_compiler->scope_depth;
  // ctx->current_compiler->scope_depth = scope_depth - 1;
  for (int i = 0; i < compiler.proto->arity; i++) {
    if (type_constraint[i].start == NULL) {
      continue;
    }
    ctx_named_variable(ctx, type_constraint[i], false);
    ctx_emit_bytes(ctx, OP_CONSTRAINT, i);
  }
  // ctx->current_compiler->scope_depth = scope_depth;

  return proto_constant;
}

static void declaration_fun(CompilerContext *ctx) {
  uint8_t global = ctx_parse_variable(ctx, "expect function name.");
  ctx_mark_initialized(ctx);
  ctx_compile_function(ctx, PROTO_FUNCTION, NULL);
  ctx_define_variable(ctx, global);
}

#define MAX_STRUCT_FIELDS 64
#define STRINGIFY(x) #x

static void declaration_struct(CompilerContext *ctx) {
  if (ctx->current_compiler->scope_depth != 0) {
    ctx_error_at(ctx, &ctx->parser.current,
                 "structs must be declared at top level.");
  }

  Token name = ctx_consume_identifier(ctx, "expect struct name.");
  int struct_name_constant = ctx_identifier_constant(ctx, &name);

  RawStructDef def;
  def.name = str_from_str(name.start, name.length, ctx->al);
  def.fields = NULL;

  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' before struct body.");

  Token field_names[MAX_STRUCT_FIELDS];
  int field_count = 0;
  while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF)) {
    if (field_count >= MAX_STRUCT_FIELDS) {
      ctx_error_at(ctx, &ctx->parser.current,
                   "can't have more than " STRINGIFY(
                       MAX_STRUCT_FIELDS) " fields in a struct.");
      while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF))
        ctx_advance(ctx);
      break;
    }
    Token field_name =
        ctx_consume_identifier(ctx, "expect field name in struct body.");
    for (int i = 0; i < field_count; i++) {
      if (identifier_equals(&field_name, &field_names[i])) {
        DIAGNOSTIC(ctx, "duplicate field name in struct body.",
                   {field_names[i], "first declared here"},
                   {field_name, "re-declared here"});
      }
    }
    field_names[field_count++] = field_name;
  }

  ctx_consume(ctx, TOKEN_RIGHT_BRACE, "expect '}' after struct body.");

  array_reserve(def.fields, field_count, ctx->al);
  for (int i = 0; i < field_count; i++) {
    String field =
        str_from_str(field_names[i].start, field_names[i].length, ctx->al);
    array_push(def.fields, field, ctx->al);
  }

  int const_idx = ctx_add_raw_constant_struct(ctx, def);
  ctx_register_definition(ctx, name, DEFKIND_STRUCT, const_idx);

  ctx_emit_bytes(ctx, OP_CONSTANT, (uint8_t)const_idx, OP_DEFINE_GLOBAL,
                 *(uint8_t *)&struct_name_constant);
}

#define MAX_TRAIT_METHODS 64

static void declaration_trait(CompilerContext *ctx) {
  if (ctx->current_compiler->scope_depth != 0) {
    ctx_error_at(ctx, &ctx->parser.current,
                 "traits must be declared at top level.");
  }

  Token name = ctx_consume_identifier(ctx, "expect trait name.");
  int trait_name_constant = ctx_identifier_constant(ctx, &name);

  RawTraitDef def;
  def.name = str_from_str(name.start, name.length, ctx->al);
  def.methods = NULL;

  Token method_names[MAX_TRAIT_METHODS];
  int method_count = 0;

  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' before trait body.");

  while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF)) {
    if (method_count >= MAX_TRAIT_METHODS) {
      ctx_error_at(ctx, &ctx->parser.current,
                   "can't have more than " STRINGIFY(
                       MAX_TRAIT_METHODS) " methods in a trait.");
      while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF))
        ctx_advance(ctx);
      break;
    }
    Token method_name_token =
        ctx_consume_identifier(ctx, "expect method name in trait body.");
    for (int i = 0; i < method_count; i++) {
      if (identifier_equals(&method_name_token, &method_names[i])) {
        DIAGNOSTIC(ctx, "duplicate method name in trait body.",
                   {method_names[i], "first declared here"},
                   {method_name_token, "re-declared here"});
      }
    }
    method_names[method_count++] = method_name_token;
  }

  ctx_consume(ctx, TOKEN_RIGHT_BRACE, "expect '}' after trait body.");

  array_reserve(def.methods, method_count, ctx->al);
  for (int i = 0; i < method_count; i++) {
    String method =
        str_from_str(method_names[i].start, method_names[i].length, ctx->al);
    array_push(def.methods, method, ctx->al);
  }

  int const_idx = ctx_add_raw_constant_trait(ctx, def);
  ctx_register_definition(ctx, name, DEFKIND_TRAIT, const_idx);

  ctx_emit_bytes(ctx, OP_CONSTANT, (uint8_t)const_idx, OP_DEFINE_GLOBAL,
                 *(uint8_t *)&trait_name_constant);
}

static void declaration_impl(CompilerContext *ctx) {
  if (ctx->current_compiler->scope_depth != 0) {
    ctx_error_at(ctx, &ctx->parser.current,
                 "impls must be declared at top level.");
  }

  ctx_consume(ctx, TOKEN_IDENTIFIER, "expect struct name after 'impl'.");
  ctx_named_variable(ctx, ctx->parser.previous, false);
  ctx_consume(ctx, TOKEN_FOR,
              "expect 'for' after struct name in impl declaration.");
  ctx_consume(ctx, TOKEN_IDENTIFIER,
              "expect trait name after 'for' in impl declaration.");
  ctx_named_variable(ctx, ctx->parser.previous, false);
  ctx_emit_byte(ctx, OP_IMPL);

  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' before impl body.");

  Token method_names[MAX_TRAIT_METHODS];
  int method_name = 0;

  while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF)) {
    ctx_consume(ctx, TOKEN_FUN, "expect method declaration in impl body.");

    if (method_name >= MAX_TRAIT_METHODS) {
      ctx_error_at(ctx, &ctx->parser.current,
                   "can't have more than " STRINGIFY(
                       MAX_TRAIT_METHODS) " methods in an impl.");
      while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF))
        ctx_advance(ctx);
      break;
    }

    for (int i = 0; i < method_name; i++) {
      if (identifier_equals(&ctx->parser.current, &method_names[i])) {
        DIAGNOSTIC(ctx, "duplicate method name in impl body.",
                   {method_names[i], "first declared here"},
                   {ctx->parser.current, "re-declared here"});
      }
    }

    ctx_consume(ctx, TOKEN_IDENTIFIER, "expect method name in impl body.");
    Token method_name_token = ctx->parser.previous;
    ctx_compile_function(ctx, PROTO_METHOD, &method_name_token);

    ctx_emit_byte(ctx, OP_IMPL_METHOD);
  }

  ctx_consume(ctx, TOKEN_RIGHT_BRACE, "expect '}' after impl body.");
  ctx_emit_byte(ctx, OP_IMPL_COMMIT);
}

// variant Result {
//     Ok(val) Err(msg)
// }

#define MAX_VARIANT_ARMS 32

static bool ctx_enforce_max_tokens(CompilerContext *ctx, int count, int max,
                                   const char *err_msg) {
  if (count >= max) {
    ctx_error_at(ctx, &ctx->parser.current, err_msg);
    return false;
  }
  return true;
}

static bool ctx_enforce_unique_token(CompilerContext *ctx, Token *new_token,
                                     Token *existing_tokens, int existing_count,
                                     const char *err_msg) {
  for (int i = 0; i < existing_count; i++) {
    if (identifier_equals(new_token, &existing_tokens[i])) {
      DIAGNOSTIC(ctx, err_msg, {existing_tokens[i], "first declared here"},
                 {*new_token, "re-declared here"});
      return false;
    }
  }
  return true;
}

static void declaration_variant(CompilerContext *ctx) {
  if (ctx->current_compiler->scope_depth != 0) {
    ctx_error_at(ctx, &ctx->parser.current,
                 "variants must be declared at top level.");
  }

  Token name = ctx_consume_identifier(ctx, "expect variant name.");
  int variant_name_constant = ctx_identifier_constant(ctx, &name);

  RawVariantDef def;
  def.name = str_from_str(name.start, name.length, ctx->al);
  def.arms = NULL;

  Token arm_names[MAX_VARIANT_ARMS];
  int arm_count = 0;
  Token arm_field_names[MAX_STRUCT_FIELDS];
  int arm_field_count = 0;

  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' before variant body.");

  while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF)) {
    const char *err_msg = "can't have more than " STRINGIFY(
        MAX_VARIANT_ARMS) " arms in a variant.";
    if (!ctx_enforce_max_tokens(ctx, arm_count, MAX_VARIANT_ARMS, err_msg)) {
      while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF)) {
        ctx_advance(ctx);
      }
      break;
    }

    Token arm_name =
        ctx_consume_identifier(ctx, "expect arm name in variant body.");
    ctx_enforce_unique_token(ctx, &arm_name, arm_field_names, arm_field_count,
                             "duplicate arm name in variant body.");

    arm_field_count = 0;
    arm_names[arm_count++] = arm_name;

    // arm
    if (ctx_match(ctx, TOKEN_LEFT_PAREN)) {
      while (!ctx_check(ctx, TOKEN_RIGHT_PAREN) && !ctx_check(ctx, TOKEN_EOF)) {
        const char *err_msg = "can't have more than " STRINGIFY(
            MAX_STRUCT_FIELDS) " fields in a variant arm.";
        if (!ctx_enforce_max_tokens(ctx, arm_field_count, MAX_STRUCT_FIELDS,
                                    err_msg)) {
          while (!ctx_check(ctx, TOKEN_RIGHT_PAREN) &&
                 !ctx_check(ctx, TOKEN_EOF)) {
            ctx_advance(ctx);
          }
          break;
        }

        Token field_name =
            ctx_consume_identifier(ctx, "expect field name in variant arm.");
        ctx_enforce_unique_token(ctx, &field_name, arm_field_names,
                                 arm_field_count,
                                 "duplicate field name in variant arm.");

        arm_field_names[arm_field_count++] = field_name;

        if (!ctx_match(ctx, TOKEN_COMMA)) {
          break;
        }
      }

      ctx_consume(ctx, TOKEN_RIGHT_PAREN,
                  "expect ')' after variant arm fields.");
    }
  }

  ctx_consume(ctx, TOKEN_RIGHT_BRACE, "expect '}' after variant body.");

  array_reserve(def.arms, arm_count, ctx->al);
  for (int i = 0; i < arm_count; i++) {
    RawVariantArm arm;
    arm.name = str_from_str(arm_names[i].start, arm_names[i].length, ctx->al);
    arm.fields = NULL;

    if (arm_field_count > 0) {
      array_reserve(arm.fields, arm_field_count, ctx->al);
      for (int j = 0; j < arm_field_count; j++) {
        String field_name = str_from_str(arm_field_names[j].start,
                                         arm_field_names[j].length, ctx->al);
        array_push(arm.fields, field_name, ctx->al);
      }
    }
    array_push(def.arms, arm, ctx->al);
  }

  int const_idx = ctx_add_raw_constant_variant(ctx, def);
  ctx_register_definition(ctx, name, DEFKIND_VARIANT, const_idx);

  ctx_emit_bytes(ctx, OP_CONSTANT, (uint8_t)const_idx, OP_DEFINE_GLOBAL,
                 *(uint8_t *)&variant_name_constant);
}

static void declaration(CompilerContext *ctx) {
  if (ctx_match(ctx, TOKEN_VAR)) {
    declaration_var(ctx);
  } else if (ctx_match(ctx, TOKEN_FUN)) {
    declaration_fun(ctx);
  } else if (ctx_match(ctx, TOKEN_STRUCT)) {
    declaration_struct(ctx);
  } else if (ctx_match(ctx, TOKEN_TRAIT)) {
    declaration_trait(ctx);
  } else if (ctx_match(ctx, TOKEN_IMPL)) {
    declaration_impl(ctx);
  } else if (ctx_match(ctx, TOKEN_VARIANT)) {
    declaration_variant(ctx);
  } else {
    statement(ctx);
  }

  if (ctx->parser.in_panic_mode) {
    ctx_synchronize(ctx);
  }
}

static void grouping(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;
  expression(ctx);
  ctx_consume(ctx, TOKEN_RIGHT_PAREN, "expect ')' after expression.");
}

int ctx_parse_argument_list(CompilerContext *ctx) {
  int arg_count = 0;
  if (!ctx_check(ctx, TOKEN_RIGHT_PAREN)) {
    do {
      expression(ctx);
      if (arg_count >= UINT8_COUNT) {
        ctx_error_at(ctx, &ctx->parser.current,
                     "can't have more than 255 arguments.");
      }
      arg_count++;
    } while (ctx_match(ctx, TOKEN_COMMA));
  }
  ctx_consume(ctx, TOKEN_RIGHT_PAREN, "expect ')' after arguments.");
  return arg_count;
}

static void call(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;
  int arg_count = ctx_parse_argument_list(ctx);
  ctx_emit_bytes(ctx, OP_CALL, (uint8_t)arg_count);
}

void number(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;
  double n = strtod(ctx->parser.previous.start, NULL);
  ctx_emit_constant(ctx, RAW_NUMBER_VALUE(n));
}

void unary(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;

  TokenType operator_type = ctx->parser.previous.type;

  // compile the operand
  parse_precedence(ctx, PREC_UNARY);

  switch (operator_type) {
  case TOKEN_BANG:
    ctx_emit_byte(ctx, OP_NOT);
    break;
  case TOKEN_MINUS:
    ctx_emit_byte(ctx, OP_NEGATE);
    break;
  default:
    assert(false && "invalid unary operator");
    return; // unreachable
  }
}

static void binary(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;

  TokenType operator_type = ctx->parser.previous.type;
  const ParseRule *rule = get_rule(operator_type);
  parse_precedence(ctx, rule->precedence + 1);

  switch (operator_type) {
  case TOKEN_BANG_EQUAL:
    ctx_emit_bytes(ctx, OP_EQUAL, OP_NOT);
    break;
  case TOKEN_EQUAL_EQUAL:
    ctx_emit_byte(ctx, OP_EQUAL);
    break;
  case TOKEN_GREATER:
    ctx_emit_byte(ctx, OP_GREATER);
    break;
  case TOKEN_GREATER_EQUAL:
    ctx_emit_bytes(ctx, OP_LESS, OP_NOT);
    break;
  case TOKEN_LESS:
    ctx_emit_byte(ctx, OP_LESS);
    break;
  case TOKEN_LESS_EQUAL: {
    ctx_emit_bytes(ctx, OP_GREATER, OP_NOT);
    break;
  }
  case TOKEN_BANG:
    ctx_emit_byte(ctx, OP_NOT);
    break;
  case TOKEN_PLUS:
    ctx_emit_byte(ctx, OP_ADD);
    break;
  case TOKEN_MINUS:
    ctx_emit_byte(ctx, OP_SUBTRACT);
    break;
  case TOKEN_STAR:
    ctx_emit_byte(ctx, OP_MULTIPLY);
    break;
  case TOKEN_SLASH:
    ctx_emit_byte(ctx, OP_DIVIDE);
    break;
  default:
    assert(false && "invalid binary operator");
    return; // unreachable
  }
}

static void literal(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;

  switch (ctx->parser.previous.type) {
  case TOKEN_FALSE:
    ctx_emit_byte(ctx, OP_FALSE);
    break;
  case TOKEN_NIL:
    ctx_emit_byte(ctx, OP_NIL);
    break;
  case TOKEN_TRUE:
    ctx_emit_byte(ctx, OP_TRUE);
    break;
  default:
    assert(false && "invalid literal token");
    return; // unreachable
  }
}

void string(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;
  int length, capacity;
  char *chars = copy_escaped_string(ctx->parser.previous.start + 1,
                                    ctx->parser.previous.length - 2, &length,
                                    &capacity, ctx->al);
  if (!chars) {
    ctx_error_at(ctx, &ctx->parser.current,
                 "out of memory for string constant.");
    return;
  }
  ctx_emit_constant(ctx, RAW_STRING_VALUE(chars, length, capacity));
}

static void variable(CompilerContext *ctx, bool can_assign) {
  ctx_named_variable(ctx, ctx->parser.previous, can_assign);
}

static void and_(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;

  int end_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);

  ctx_emit_byte(ctx, OP_POP);
  parse_precedence(ctx, PREC_AND);

  ctx_patch_jump(ctx, end_jump);
}

static void or_(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;

  int else_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);
  int end_jump = ctx_emit_jump(ctx, OP_JUMP);

  ctx_patch_jump(ctx, else_jump);
  ctx_emit_byte(ctx, OP_POP);

  parse_precedence(ctx, PREC_OR);
  ctx_patch_jump(ctx, end_jump);
}

static void dot(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;

  ctx_consume(ctx, TOKEN_IDENTIFIER, "expect field name after '.'.");
  uint8_t name = ctx_identifier_constant(ctx, &ctx->parser.previous);

  // [op_code, name_constant, def_id, def_id, offset]
  // in runtime the vm will def_id and offset for field
  // access optimization

  if (can_assign && ctx_match(ctx, TOKEN_EQUAL)) {
    expression(ctx);
    ctx_emit_bytes(ctx, OP_SET_PROPERTY, name, 0, 0, 0);
  } else if (ctx_match(ctx, TOKEN_LEFT_PAREN)) {
    int arg_count = ctx_parse_argument_list(ctx);
    // name_const + 2 bytes trait_id + 1 byte slot = 4 bytes padding
    ctx_emit_bytes(ctx, OP_CALL_METHOD, name, 0, 0, 0, arg_count);
  } else {
    ctx_emit_bytes(ctx, OP_GET_PROPERTY, name, 0, 0, 0);
  }
}

static void as(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;

  expression(ctx);
  ctx_emit_byte(ctx, OP_CAST_TRAIT);
}

static void if_(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;
  expression(ctx);

  int else_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);
  ctx_emit_byte(ctx, OP_POP);

  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' after 'if' expression.");
  expression(ctx);
  ctx_consume(ctx, TOKEN_RIGHT_BRACE, "expect '}' after if expression.");

  int exit_jump = ctx_emit_jump(ctx, OP_JUMP);
  ctx_patch_jump(ctx, else_jump);
  ctx_emit_byte(ctx, OP_POP);

  ctx_consume(ctx, TOKEN_ELSE, "expect 'else' after 'if' expression.");

  if (ctx_match(ctx, TOKEN_IF)) {
    if_(ctx, can_assign); // else if
    ctx_patch_jump(ctx, exit_jump);
    return;
  }

  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' after 'else'.");
  expression(ctx);
  ctx_consume(ctx, TOKEN_RIGHT_BRACE, "expect '}' after else expression.");

  ctx_patch_jump(ctx, exit_jump);
}

static void fun(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;

  Token name;
  bool is_named = false;

  if (ctx_match(ctx, TOKEN_IDENTIFIER)) {
    name = ctx->parser.previous;
    is_named = true;
  }

  ctx_compile_function(ctx, PROTO_FUNCTION, is_named ? &name : NULL);
}

static void match(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;
  ctx_compile_match(ctx, MATCH_MODE_EXPRESSION);
}

Proto *compile(const char *source, Allocator *al) {
  CompilerContext ctx;
  ctx_init(&ctx, source, al);

  Compiler compiler;
  ctx_begin_compile(&ctx, &compiler, PROTO_SCRIPT);

  ctx_advance(&ctx);

  while (!ctx_match(&ctx, TOKEN_EOF)) {
    declaration(&ctx);
  }

  Proto *proto = ctx_end_compile(&ctx);
  if (ctx.parser.had_error) {
    al_free_for(al, proto);
    ctx_destroy(&ctx);
    return NULL;
  }

  ctx_destroy(&ctx);
  return proto;
}
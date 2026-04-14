#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "compiler.h"
#include "dynamic_array.h"
#include "memory.h"
#include "opcode.h"
#include "scanner.h"

#define MAX_ASSIGNMENTS                                                        \
  8 // maximum number of variables in a multiple assignment statement, e.g. `var
    // a, b, c = 1, 2, 3`
#define UINT8_COUNT UINT8_MAX + 1

void proto_init(Proto *proto, ProtoType type, char *name, Allocator *al) {
  proto->type = type;
  proto->name = name;
  proto->code = array_new(al, uint8_t);
  proto->constants = array_new(al, RawConstant);
  proto->lines = array_new(al, int);
}

static void constant_destory(RawConstant constant, Allocator *al) {
  if (constant.type == RAW_STRING && constant.as.string.chars != NULL) {
    al_free(al, constant.as.string.chars, constant.as.string.capacity + 1);
  }
}

void proto_destroy(Proto *proto, Allocator *al) {
  int constant_count = array_count(proto->constants);
  for (int i = 0; i < constant_count; i++) {
    RawConstant constant = proto->constants[i];
    constant_destory(constant, al);
  }
  if (proto->name != NULL) {
    al_free(al, proto->name, strlen(proto->name) + 1);
  }
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
} Local;

typedef struct Compiler {
  struct Compiler *enclosing;

  int scope_depth;
  int local_count;
  Local locals[UINT8_COUNT];

  Proto *proto;
} Compiler;

void compiler_init(Compiler *compiler, ProtoType type, Allocator *al) {
  compiler->enclosing = NULL;
  compiler->scope_depth = 0;
  compiler->local_count = 0;

  proto_init(compiler->proto, type, NULL, al);
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

  Allocator *al;
} CompilerContext;

static void ctx_init(CompilerContext *ctx, const char *source, Allocator *al) {
  ctx->source = source;
  parser_init(&ctx->parser, source);
  ctx->current_loop = NULL;
  ctx->current_compiler = NULL;
  ctx->al = al;
}

// for speculative parsing
// typedef struct {
//   Parser parser;
//   size_t constants_count;
// } Checkpoint;

// static Checkpoint ctx_checkpoint(CompilerContext *ctx) {
//   return (Checkpoint){.parser = ctx->parser,
//                       .constants_count =
//                           array_count(ctx->current_compiler->proto.constants)};
// }

// static void ctx_restore(CompilerContext *ctx, Checkpoint cp) {
//   ctx->parser = cp.parser;
//   while (array_count(ctx->current_compiler->proto.constants) >
//          cp.constants_count) {
//     RawConstant constant = array_pop(ctx->current_compiler->proto.constants);
//     constant_destory(constant, ctx->al);
//   }
// }

static char *copy_string(const char *str, int length, Allocator *al) {
  char *copy = al_alloc(al, length + 1);
  memcpy(copy, str, length);
  copy[length] = '\0';
  return copy;
}

static char *copy_escaped_string(const char *str, int length, int *out_length,
                                 int *out_capacity, Allocator *al) {
  int capacity = length;
  char *chars = al_alloc(al, capacity + 1);
  int j = 0;
  for (int i = 0; i < length; i++) {
    if (str[i] == '\\' && i + 1 < length) {
      i++;
      switch (str[i]) {
      case 'n':
        chars[j++] = '\n';
        break;
      case 'r':
        chars[j++] = '\r';
        break;
      case 't':
        chars[j++] = '\t';
        break;
      case '\\':
        chars[j++] = '\\';
        break;
      case '"':
        chars[j++] = '"';
        break;
      default:
        chars[j++] = str[i];
        break;
      }
    } else {
      chars[j++] = str[i];
    }
  }
  chars[j] = '\0';
  if (out_length) {
    *out_length = j;
  }
  if (out_capacity) {
    *out_capacity = capacity;
  }
  return chars;
}

static void ctx_begin_compile(CompilerContext *ctx, Compiler *compiler,
                              ProtoType type) {
  compiler_init(compiler, type, ctx->al);

  compiler->enclosing = ctx->current_compiler;
  ctx->current_compiler = compiler;

  compiler->proto = al_alloc_for(ctx->al, Proto);

  if (type != PROTO_SCRIPT) {
    compiler->proto->name = copy_string(ctx->parser.previous.start,
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

static void ctx_emit_constant(CompilerContext *ctx, RawConstant value) {
  ctx_emit_bytes(ctx, OP_CONSTANT, ctx_make_constant(ctx, value));
}

static void ctx_emit_return(CompilerContext *ctx) {
  ctx_emit_byte(ctx, OP_RETURN);
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
    // if (ctx->parser.current.type == TOKEN_SEMICOLON) {
    //   ctx_advance(ctx);
    //   return;
    // }

    switch (ctx->parser.current.type) {
    case TOKEN_RIGHT_BRACE:
    case TOKEN_CLASS:
    case TOKEN_FUN:
    case TOKEN_VAR:
    case TOKEN_FOR:
    case TOKEN_IF:
    case TOKEN_PRINT:
    case TOKEN_RETURN:
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
static void number(CompilerContext *ctx, bool can_assign);
static void unary(CompilerContext *ctx, bool can_assign);
static void binary(CompilerContext *ctx, bool can_assign);
static void literal(CompilerContext *ctx, bool can_assign);
static void string(CompilerContext *ctx, bool can_assign);
static void variable(CompilerContext *ctx, bool can_assign);
static void and_(CompilerContext *ctx, bool can_assign);
static void or_(CompilerContext *ctx, bool can_assign);

static void if_(CompilerContext *ctx, bool can_assign);

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN] = {grouping, NULL, PREC_NONE},
    [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},
    [TOKEN_LEFT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_COLON] = {NULL, NULL, PREC_NONE},
    [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
    [TOKEN_DOT] = {NULL, NULL, PREC_NONE},
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

    [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
    [TOKEN_STRING] = {string, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},

    [TOKEN_AND] = {NULL, and_, PREC_AND},
    [TOKEN_BREAK] = {NULL, NULL, PREC_NONE},
    [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
    [TOKEN_CONTINUE] = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
    [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
    [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
    [TOKEN_FUN] = {NULL, NULL, PREC_NONE},
    [TOKEN_IF] = {if_, NULL, PREC_NONE},
    [TOKEN_NIL] = {literal, NULL, PREC_NONE},
    [TOKEN_OR] = {NULL, or_, PREC_OR},
    [TOKEN_PRINT] = {NULL, NULL, PREC_NONE},
    [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},
    [TOKEN_SUPER] = {NULL, NULL, PREC_NONE},
    [TOKEN_THIS] = {NULL, NULL, PREC_NONE},
    [TOKEN_TRUE] = {literal, NULL, PREC_NONE},
    [TOKEN_VAR] = {NULL, NULL, PREC_NONE},

    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
};

const ParseRule *get_rule(TokenType type) { return &rules[type]; }

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
    ctx_error_at(ctx, &ctx->parser.current, "invalid assignment target.");
  }
}

static void expression(CompilerContext *ctx) {
  parse_precedence(ctx, PREC_ASSIGNMENT);
}

static bool identifier_equals(Token *a, Token *b);
static uint8_t ctx_identifier_constant(CompilerContext *ctx, Token *name);

int ctx_resolve_local(CompilerContext *ctx, Token *name) {
  for (int i = ctx->current_compiler->local_count - 1; i >= 0; i--) {
    Local *local = &ctx->current_compiler->locals[i];
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

static VarRef ctx_resolve_variable(CompilerContext *ctx, Token name) {
  VarRef ref;
  if ((ref.arg = ctx_resolve_local(ctx, &name)) > -1) {
    ref.op_get = OP_GET_LOCAL;
    ref.op_set = OP_SET_LOCAL;
  } else {
    ref.arg = ctx_identifier_constant(ctx, &name);
    ref.op_get = OP_GET_GLOBAL;
    ref.op_set = OP_SET_GLOBAL;
  }
  return ref;
}

void ctx_named_variable(CompilerContext *ctx, Token name, bool can_assign) {
  VarRef ref = ctx_resolve_variable(ctx, name);

  if (can_assign && ctx_match(ctx, TOKEN_EQUAL)) {
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

static void statement_print(CompilerContext *ctx) {
  expression(ctx);
  ctx_emit_byte(ctx, OP_PRINT);
}

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
    ctx_emit_byte(ctx, OP_POP);
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

static void statement(CompilerContext *ctx) {
  if (ctx_match(ctx, TOKEN_PRINT)) {
    statement_print(ctx);
  } else if (ctx_match(ctx, TOKEN_IF)) {
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
  } else {
    statement_expression(ctx);
  }
}

static uint8_t ctx_identifier_constant(CompilerContext *ctx, Token *name) {
  char *name_str = copy_string(name->start, name->length, ctx->al);
  return ctx_make_constant(
      ctx, RAW_STRING_VALUE(name_str, name->length, name->length));
}

bool identifier_equals(Token *a, Token *b) {
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
  uint8_t globals[MAX_ASSIGNMENTS];
  globals[0] = ctx_parse_variable(ctx, "expect variable name");
  int assignments = 1;

  while (ctx_match(ctx, TOKEN_COMMA)) {
    if (assignments >= MAX_ASSIGNMENTS) {
      ctx_error_at(ctx, &ctx->parser.previous,
                   "too many variables in multiple assignment.");
      break;
    }
    globals[assignments++] = ctx_parse_variable(ctx, "expect variable name");
  }

  if (ctx_match(ctx, TOKEN_EQUAL)) {
    for (int i = 0; i < assignments; i++) {
      bool ok = true;
      if (i > 0) {
        ok = ctx_consume(ctx, TOKEN_COMMA, "expect ',' between initializers");
      }
      if (ok) {
        expression(ctx);
      }
    }
  } else {
    for (int i = 0; i < assignments; i++) {
      ctx_emit_byte(ctx, OP_NIL);
    }
  }

  for (int i = 0; i < assignments; i++) {
    uint8_t index = globals[i];
    ctx_define_variable(ctx, index);
  }
}

static void declaration(CompilerContext *ctx) {
  if (ctx_match(ctx, TOKEN_VAR)) {
    declaration_var(ctx);
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

void binary(CompilerContext *ctx, bool can_assign) {
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

void literal(CompilerContext *ctx, bool can_assign) {
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

void variable(CompilerContext *ctx, bool can_assign) {
  ctx_named_variable(ctx, ctx->parser.previous, can_assign);
}

void and_(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;

  int end_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);

  ctx_emit_byte(ctx, OP_POP);
  parse_precedence(ctx, PREC_AND);

  ctx_patch_jump(ctx, end_jump);
}

void or_(CompilerContext *ctx, bool can_assign) {
  (void)can_assign;

  int else_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);
  int end_jump = ctx_emit_jump(ctx, OP_JUMP);

  ctx_patch_jump(ctx, else_jump);
  ctx_emit_byte(ctx, OP_POP);

  parse_precedence(ctx, PREC_OR);
  ctx_patch_jump(ctx, end_jump);
}

void if_(CompilerContext *ctx, bool can_assign) {
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
    return NULL;
  }

  return proto;
}
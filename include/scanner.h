#pragma once

typedef struct {
  const char *start;
  const char *current;
  int line;
} Scanner;

typedef enum {
  TOKEN_LEFT_PAREN,
  TOKEN_RIGHT_PAREN,
  TOKEN_LEFT_BRACE,
  TOKEN_RIGHT_BRACE,
  TOKEN_COLON,
  TOKEN_COMMA,
  TOKEN_DOT,
  TOKEN_MINUS,
  TOKEN_MINUS_MINUS,
  TOKEN_PLUS,
  TOKEN_PLUS_PLUS,
  TOKEN_SEMICOLON,
  TOKEN_SLASH,
  TOKEN_STAR,

  TOKEN_BANG,
  TOKEN_BANG_EQUAL,
  TOKEN_EQUAL,
  TOKEN_EQUAL_EQUAL,
  TOKEN_GREATER,
  TOKEN_GREATER_EQUAL,
  TOKEN_LESS,
  TOKEN_LESS_EQUAL,
  TOKEN_FAT_ARROW, // =>

  TOKEN_IDENTIFIER,
  TOKEN_STRING,
  TOKEN_NUMBER,

  TOKEN_AND,
  TOKEN_AS,
  TOKEN_BREAK,
  // TOKEN_CLASS,
  TOKEN_CONTINUE,
  TOKEN_ELSE,
  TOKEN_FALSE,
  TOKEN_FOR,
  TOKEN_FUN,
  TOKEN_IF,
  TOKEN_IS,
  TOKEN_IMPL,
  TOKEN_MATCH,
  TOKEN_NIL,
  TOKEN_OR,
  // TOKEN_PRINT,
  TOKEN_RETURN,
  // TOKEN_SUPER,
  TOKEN_STRUCT,
  TOKEN_THIS,
  TOKEN_TRAIT,
  TOKEN_TRUE,
  TOKEN_VAR,

  TOKEN_ERROR,
  TOKEN_EOF
} TokenType;

typedef struct {
  TokenType type;
  const char *start;
  int length;

  int line_number;
  const char *source; // pointer to the start of the token in the source,
                      // used for error reporting
} Token;

Token token_from_cstr(TokenType type, const char *c_str);

void scanner_init(Scanner *scanner, const char *source);

// scan the next token from the source and return it
Token scanner_next_token(Scanner *scanner);

Token scanner_peek_next_token(Scanner *s);
#pragma once

#include <string>

#define OPERATORS_COUNT 23
#define KEYWORDS_COUNT 40

typedef enum {
    TOKEN_INT,
    TOKEN_FLOAT,
    TOKEN_BOOL,
    TOKEN_STRING,
    TOKEN_IDENT,
    TOKEN_KW,
    TOKEN_OP,
    TOKEN_NEWLINE,
    TOKEN_INDENT,
    TOKEN_DEDENT,
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

typedef enum {
    KW_AND, KW_ARRAY, KW_AS,
    KW_BOOL, KW_BREAK, KW_CONTINUE,
    KW_DEF, KW_ELSE, KW_EXIT,
    KW_FALSE, KW_FLOAT32, KW_FLOAT64,
    KW_IF, KW_IMPL, KW_INPUT,
    KW_INT16, KW_INT32, KW_INT64, KW_INT8,
    KW_LEN, KW_LET, KW_NAMESPACE,
    KW_NOT, KW_OR, KW_PANIC, KW_PRINT,
    KW_RETURN, KW_SIZEOF, KW_STRING, KW_STRUCT,
    KW_TRUE, KW_TYPE, KW_TYPEOF,
    KW_UINT16, KW_UINT32, KW_UINT64, KW_UINT8,
    KW_VAR, KW_VOID, KW_WHILE,
    KW_NONE
} KeywordType;

typedef enum {
    OP_EQEQ, OP_NEQ, OP_LEQ, OP_GEQ, OP_ARROW, OP_DCOLON,
    OP_PLUS, OP_MINUS, OP_STAR, OP_SLASH, OP_PERCENT,
    OP_LT, OP_GT, OP_EQ,
    OP_COLON, OP_COMMA, OP_DOT,
    OP_LPAREN, OP_RPAREN,
    OP_LBRACK, OP_RBRACK,
    OP_LBRACE, OP_RBRACE,
    OP_NONE
} OpType;

typedef struct {
    std::string lexeme;
    TokenType type;
    KeywordType kw;
    OpType op;
    int line;
    int col;
} Token;

#pragma once

#include "token.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

typedef enum { // типы данных
    TYPE_INT8, TYPE_INT16, TYPE_INT32, TYPE_INT64,
    TYPE_UINT8, TYPE_UINT16, TYPE_UINT32, TYPE_UINT64,
    TYPE_FLOAT32, TYPE_FLOAT64,
    TYPE_BOOL, TYPE_STRING, TYPE_VOID,
    TYPE_ARRAY,
    TYPE_NAMED,
} TypeKind;

typedef struct Type {
    TypeKind kind;
    std::string name; // имя 
    std::unique_ptr<Type> elem; // тип элемента
    int array_size; // для массива
    int line, col;
} Type;

typedef enum { // тип выражения
    EXPR_INT, EXPR_FLOAT, EXPR_BOOL, EXPR_STRING, EXPR_IDENT,
    EXPR_BINARY, EXPR_UNARY,
    EXPR_CAST,
    EXPR_SIZEOF, // A.1.13: sizeof(type) -- cast_type хранит тип
    EXPR_TYPEOF, // A.1.13: typeof(expr) -- lhs хранит выражение
    EXPR_INDEX,
    EXPR_FIELD,
    EXPR_SCOPE,
    EXPR_CALL,
    EXPR_ARRAY,
    EXPR_STRUCT,
} ExprKind;

typedef enum { // бинарные операторы
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD,
    BINOP_EQ, BINOP_NEQ, BINOP_LT, BINOP_GT, BINOP_LEQ, BINOP_GEQ,
    BINOP_AND, BINOP_OR,
} BinOp;

typedef enum { // унарные операторы
    UNOP_NEG,
    UNOP_NOT,
} UnOp;

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

typedef struct FieldInit { // для структур
    std::string name;
    ExprPtr value;
} FieldInit;

typedef struct Expr { // одна структура для всех выражений
    ExprKind kind;
    int line, col;

    int64_t ival;
    double fval;
    bool bval;
    std::string sval;

    ExprPtr lhs, rhs;
    BinOp binop;
    UnOp unop;
    Type cast_type;

    std::vector<ExprPtr> args;
    std::vector<ExprPtr> elems;
    std::vector<FieldInit> fields;
} Expr;

typedef enum {
    STMT_VAR, // переменная
    STMT_ASSIGN, // присваивание
    STMT_IF,
    STMT_WHILE,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_EXPR,
} StmtKind;

struct Stmt;
using Block = std::vector<std::unique_ptr<Stmt>>; // тело цикла / условия

typedef struct Stmt { // ?
    StmtKind kind;
    int line, col;

    bool is_mut; // мутабельность
    std::string var_name;
    bool has_type; // явный тип
    Type var_type;
    ExprPtr init; // выражение

    ExprPtr target, val; // ?

    ExprPtr cond;
    Block body;
    Block else_body;
    bool has_else;

    ExprPtr ret_val;

    ExprPtr expr;
} Stmt;

typedef struct Param { // параметры функции: имя и тип
    std::string name;
    Type type;
    int line, col;
} Param;

typedef struct FieldDecl { // для одного поля структуры - имя + тип
    std::string name;
    Type type;
    int line, col;
} FieldDecl;

typedef enum {
    DECL_FUNC,
    DECL_STRUCT,
    DECL_TYPE_ALIAS, // typedef/using короче имя
    DECL_NAMESPACE,
    DECL_IMPL, // методы структуры
} DeclKind;

struct Decl;
using DeclList = std::vector<std::unique_ptr<Decl>>; // указатели на объявления

typedef struct Decl {
    DeclKind kind;
    int line, col;
    std::string name; // ?
    std::string struct_name; // DECL_IMPL: имя структуры

    std::vector<Param> params;
    Type ret_type;
    Block body;

    std::vector<FieldDecl> fields;

    Type alias_type;

    DeclList decls;
} Decl;

typedef struct Program {
    DeclList decls;
} Program;

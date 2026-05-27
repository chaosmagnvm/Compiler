#include "parser.hpp"
#include <charconv>
#include <string>

struct Parser {
    const std::vector<Token> *tokens;
    int pos;
    ParseError err;
    bool had_error;
};

static const Token &cur(Parser *p) {
    int n = (int)p->tokens->size();
    return (*p->tokens)[p->pos < n ? p->pos : n - 1];
}

static bool check(Parser *p, TokenType t) { return cur(p).type == t; }
static bool check_kw(Parser *p, KeywordType k) { return cur(p).type == TOKEN_KW && cur(p).kw == k; }
static bool check_op(Parser *p, OpType o) { return cur(p).type == TOKEN_OP && cur(p).op == o; }

static const Token &advance(Parser *p) {
    const Token &t = cur(p);
    if (p->pos < (int)p->tokens->size() - 1) p->pos++;
    return t;
}

static bool match_op(Parser *p, OpType o) {
    if (check_op(p, o)) { advance(p); return true; }
    return false;
}

static std::string describe(const Token &t) {
    switch (t.type) {
    case TOKEN_NEWLINE:
        return "newline";
    case TOKEN_INDENT:
        return "indent";
    case TOKEN_DEDENT:
        return "dedent";
    case TOKEN_EOF:
        return "end of file";
    default:
        return "'" + t.lexeme + "'";
    }
}

static void fail(Parser *p, const std::string &msg) {
    if (p->had_error) return;
    p->err = {msg, cur(p).line, cur(p).col};
    p->had_error = true;
}

static bool expect(Parser *p, TokenType t, const char *what) {
    if (check(p, t)) { advance(p); return true; }
    fail(p, std::string("expected ") + what + ", got " + describe(cur(p)));
    return false;
}

static bool expect_op(Parser *p, OpType o, const char *sym) {
    if (match_op(p, o)) return true;
    fail(p, std::string("expected '") + sym + "', got " + describe(cur(p)));
    return false;
}

static bool expect_ident(Parser *p, std::string *out) {
    if (!check(p, TOKEN_IDENT)) {
        fail(p, "expected identifier, got " + describe(cur(p)));
        return false;
    }
    *out = advance(p).lexeme;
    return true;
}

static bool parse_type(Parser *p, Type *out);
static ExprPtr parse_expr(Parser *p);
static bool parse_block(Parser *p, Block *out);
static std::unique_ptr<Stmt> parse_stmt(Parser *p);
static std::unique_ptr<Decl> parse_decl(Parser *p);

static bool parse_type(Parser *p, Type *out) {
    out->line = cur(p).line;
    out->col = cur(p).col;

    switch (cur(p).kw) {
    case KW_INT8:
        out->kind = TYPE_INT8;
        advance(p);
        return true;
    case KW_INT16:
        out->kind = TYPE_INT16;
        advance(p);
        return true;
    case KW_INT32:
        out->kind = TYPE_INT32;
        advance(p);
        return true;
    case KW_INT64:
        out->kind = TYPE_INT64;
        advance(p);
        return true;
    case KW_UINT8:
        out->kind = TYPE_UINT8;
        advance(p);
        return true;
    case KW_UINT16:
        out->kind = TYPE_UINT16;
        advance(p);
        return true;
    case KW_UINT32:
        out->kind = TYPE_UINT32;
        advance(p);
        return true;
    case KW_UINT64:
        out->kind = TYPE_UINT64;
        advance(p);
        return true;
    case KW_FLOAT32:
        out->kind = TYPE_FLOAT32;
        advance(p);
        return true;
    case KW_FLOAT64:
        out->kind = TYPE_FLOAT64;
        advance(p);
        return true;
    case KW_BOOL:
        out->kind = TYPE_BOOL;
        advance(p);
        return true;
    case KW_STRING:
        out->kind = TYPE_STRING;
        advance(p);
        return true;
    case KW_VOID:
        out->kind = TYPE_VOID;
        advance(p);
        return true;
    default:
        break;
    }

    if (check_kw(p, KW_ARRAY)) {
        advance(p);
        if (!expect_op(p, OP_LBRACK, "[")) return false;
        out->kind = TYPE_ARRAY;
        out->elem = std::make_unique<Type>();
        if (!parse_type(p, out->elem.get())) return false;
        if (!expect_op(p, OP_COMMA, ",")) return false;
        if (!check(p, TOKEN_INT)) {
            fail(p, "expected array size");
            return false;
        }
        std::from_chars(cur(p).lexeme.data(),
                        cur(p).lexeme.data() + cur(p).lexeme.size(),
                        out->array_size);
        advance(p);
        return expect_op(p, OP_RBRACK, "]");
    }

    if (check(p, TOKEN_IDENT)) {
        out->kind = TYPE_NAMED;
        out->name = advance(p).lexeme;
        return true;
    }

    fail(p, "expected type, got " + describe(cur(p)));
    return false;
}

static ExprPtr make_expr(ExprKind kind, int line, int col) {
    auto e = std::make_unique<Expr>();
    e->kind = kind;
    e->line = line;
    e->col = col;
    e->ival = 0;
    e->fval = 0.0;
    e->bval = false;
    e->binop = BINOP_ADD;
    e->unop = UNOP_NEG;
    return e;
}

static bool is_lvalue(const Expr &e) {
    return e.kind == EXPR_IDENT || e.kind == EXPR_INDEX || e.kind == EXPR_FIELD;
}

static ExprPtr parse_or_expr(Parser *p); // or
static ExprPtr parse_and_expr(Parser *p); // and
static ExprPtr parse_not_expr(Parser *p); // not
static ExprPtr parse_cmp_expr(Parser *p); // == != < > <= >=
static ExprPtr parse_add_expr(Parser *p); // +-
static ExprPtr parse_mul_expr(Parser *p); // * / %
static ExprPtr parse_unary_expr(Parser *p); // - (unary)
static ExprPtr parse_cast_expr(Parser *p); // cast
static ExprPtr parse_postfix_expr(Parser *p); //[] . :: ()
static ExprPtr parse_primary_expr(Parser *p);

static ExprPtr parse_expr(Parser *p) { return parse_or_expr(p); }

static ExprPtr parse_or_expr(Parser *p) {
    ExprPtr lhs = parse_and_expr(p);
    while (lhs && !p->had_error && check_kw(p, KW_OR)) {
        int line = cur(p).line, col = cur(p).col;
        advance(p);
        ExprPtr rhs = parse_and_expr(p);
        if (!rhs) return nullptr;
        auto e = make_expr(EXPR_BINARY, line, col);
        e->binop = BINOP_OR;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        lhs = std::move(e);
    }
    return lhs;
}

static ExprPtr parse_and_expr(Parser *p) {
    ExprPtr lhs = parse_not_expr(p);
    while (lhs && !p->had_error && check_kw(p, KW_AND)) { // пока левая часть
        int line = cur(p).line, col = cur(p).col;
        advance(p);
        ExprPtr rhs = parse_not_expr(p); // рекурсия из-за правой асоциативности. второй not важнее первого
        if (!rhs) return nullptr;
        auto e = make_expr(EXPR_BINARY, line, col); // создаем бинарное выражение
        e->binop = BINOP_AND;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        lhs = std::move(e);
    }
    return lhs;
}

static ExprPtr parse_not_expr(Parser *p) { // унарный оператор не
    if (check_kw(p, KW_NOT)) {
        int line = cur(p).line, col = cur(p).col;
        advance(p);
        ExprPtr operand = parse_not_expr(p);
        if (!operand) return nullptr;
        auto e = make_expr(EXPR_UNARY, line, col);
        e->unop = UNOP_NOT;
        e->lhs = std::move(operand);
        return e;
    }
    return parse_cmp_expr(p);
}

static ExprPtr parse_cmp_expr(Parser *p) { // сравнения
    ExprPtr lhs = parse_add_expr(p); // парсим сложения
    if (!lhs || p->had_error) return lhs;

    int line = cur(p).line, col = cur(p).col;
    BinOp binop;

    if (check_op(p, OP_EQEQ)) 
        binop = BINOP_EQ;
    else if (check_op(p, OP_NEQ)) 
        binop = BINOP_NEQ;
    else if (check_op(p, OP_LEQ)) 
        binop = BINOP_LEQ;
    else if (check_op(p, OP_GEQ)) 
        binop = BINOP_GEQ;
    else if (check_op(p, OP_LT)) 
        binop = BINOP_LT;
    else if (check_op(p, OP_GT)) 
        binop = BINOP_GT;
    else return lhs;

    advance(p);
    ExprPtr rhs = parse_add_expr(p);
    if (!rhs) return nullptr;
    auto e = make_expr(EXPR_BINARY, line, col);
    e->binop = binop;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

static ExprPtr parse_add_expr(Parser *p) {
    ExprPtr lhs = parse_mul_expr(p); // спускаемся ниже
    while (lhs && !p->had_error && (check_op(p, OP_PLUS) || check_op(p, OP_MINUS))) { // нужный оператор
        int line = cur(p).line, col = cur(p).col;
        BinOp op = check_op(p, OP_PLUS) ? BINOP_ADD : BINOP_SUB;
        advance(p); // запомнил оператор + сдвиг
        ExprPtr rhs = parse_mul_expr(p); // правая часть - ниже
        if (!rhs) return nullptr;
        auto e = make_expr(EXPR_BINARY, line, col);
        e->binop = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        lhs = std::move(e); // создаем дерево
    }
    return lhs;
}

static ExprPtr parse_mul_expr(Parser *p) {
    ExprPtr lhs = parse_unary_expr(p);
    while (lhs && !p->had_error &&
           (check_op(p, OP_STAR) || check_op(p, OP_SLASH) || check_op(p, OP_PERCENT))) {
        int line = cur(p).line, col = cur(p).col;
        BinOp op;
        if (check_op(p, OP_STAR)) op = BINOP_MUL;
        else if (check_op(p, OP_SLASH)) op = BINOP_DIV;
        else op = BINOP_MOD;
        advance(p);
        ExprPtr rhs = parse_unary_expr(p);
        if (!rhs) return nullptr;
        auto e = make_expr(EXPR_BINARY, line, col);
        e->binop = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        lhs = std::move(e);
    }
    return lhs;
}

static ExprPtr parse_unary_expr(Parser *p) {
    if (check_op(p, OP_MINUS)) {
        int line = cur(p).line, col = cur(p).col;
        advance(p);
        ExprPtr operand = parse_unary_expr(p); // если есть еще один унарный минус
        if (!operand) return nullptr;
        auto e = make_expr(EXPR_UNARY, line, col);
        e->unop = UNOP_NEG;
        e->lhs = std::move(operand);
        return e;
    }
    return parse_cast_expr(p);
}

static ExprPtr parse_cast_expr(Parser *p) {
    ExprPtr e = parse_postfix_expr(p);
    if (!e || p->had_error) return e;
    if (check_kw(p, KW_AS)) {
        int line = e->line, col = e->col;
        advance(p);
        auto cast = make_expr(EXPR_CAST, line, col);
        if (!parse_type(p, &cast->cast_type)) return nullptr;
        cast->lhs = std::move(e);
        return cast;
    }
    return e;
}

static ExprPtr parse_postfix_expr(Parser *p) {
    ExprPtr base = parse_primary_expr(p);
    if (!base || p->had_error) return base;

    while (!p->had_error) {
        int line = cur(p).line, col = cur(p).col;

        if (check_op(p, OP_LBRACK)) {
            advance(p);
            ExprPtr idx = parse_expr(p); // право
            if (!idx) return nullptr;
            if (!expect_op(p, OP_RBRACK, "]")) return nullptr;
            auto e = make_expr(EXPR_INDEX, line, col);
            e->lhs = std::move(base);
            e->rhs = std::move(idx);
            base = std::move(e); // новый base (для вложенностей)
            continue;
        }

        if (check_op(p, OP_DOT)) {
            advance(p);
            std::string field;
            if (!expect_ident(p, &field)) return nullptr;
            auto e = make_expr(EXPR_FIELD, line, col);
            e->lhs = std::move(base);
            e->sval = std::move(field);
            base = std::move(e);
            continue;
        }

        if (check_op(p, OP_DCOLON)) {
            advance(p);
            std::string member;
            if (!expect_ident(p, &member)) return nullptr;
            auto e = make_expr(EXPR_SCOPE, line, col);
            e->lhs = std::move(base);
            e->sval = std::move(member);
            base = std::move(e);
            continue;
        }

        if (check_op(p, OP_LPAREN)) { // скобка - функция
            advance(p);
            auto call = make_expr(EXPR_CALL, line, col);
            call->lhs = std::move(base);
            if (!check_op(p, OP_RPAREN)) { // аргументы функции
                ExprPtr arg = parse_expr(p);
                if (!arg) return nullptr;
                call->args.push_back(std::move(arg));
                while (!p->had_error && match_op(p, OP_COMMA)) {
                    arg = parse_expr(p);
                    if (!arg) return nullptr;
                    call->args.push_back(std::move(arg)); // аргументы функции
                }
            }
            if (!expect_op(p, OP_RPAREN, ")")) return nullptr;
            base = std::move(call);
            continue;
        }

        break;
    }
    return base;
}

static bool is_builtin_kw(KeywordType k) { // нужное ли ключевое слово?
    return k == KW_PRINT || k == KW_INPUT || k == KW_EXIT || k == KW_PANIC || k == KW_LEN;
}

static ExprPtr parse_primary_expr(Parser *p) {
    int line = cur(p).line, col = cur(p).col;

    if (check(p, TOKEN_INT)) {
        auto e = make_expr(EXPR_INT, line, col);
        std::from_chars(cur(p).lexeme.data(), cur(p).lexeme.data() + cur(p).lexeme.size(), e->ival);
        advance(p);
        return e;
    }

    if (check(p, TOKEN_FLOAT)) {
        auto e = make_expr(EXPR_FLOAT, line, col);
        e->fval = std::stod(cur(p).lexeme);
        advance(p);
        return e;
    }

    if (check(p, TOKEN_STRING)) {
        auto e = make_expr(EXPR_STRING, line, col);
        e->sval = advance(p).lexeme;
        return e;
    }

    if (check_kw(p, KW_TRUE) || check_kw(p, KW_FALSE)) {
        auto e = make_expr(EXPR_BOOL, line, col);
        e->bval = (cur(p).kw == KW_TRUE);
        advance(p);
        return e;
    }

    if (cur(p).type == TOKEN_KW && is_builtin_kw(cur(p).kw)) {
        auto e = make_expr(EXPR_IDENT, line, col);
        e->sval = advance(p).lexeme;
        return e;
    }

    if (check(p, TOKEN_IDENT)) {
        std::string name = advance(p).lexeme;

        if (check_op(p, OP_LBRACE)) { // после имени - инициализация структуры
            advance(p);
            auto e = make_expr(EXPR_STRUCT, line, col);
            e->sval = std::move(name);
            while (!p->had_error && !check_op(p, OP_RBRACE) && !check(p, TOKEN_EOF)) {
                std::string fname;
                if (!expect_ident(p, &fname)) return nullptr; // имя поля
                if (!expect_op(p, OP_COLON, ":")) return nullptr;
                ExprPtr val = parse_expr(p);
                if (!val) return nullptr;
                e->fields.push_back({std::move(fname), std::move(val)}); // поля структуры
                if (!check_op(p, OP_RBRACE)) {
                    if (!expect_op(p, OP_COMMA, ",")) return nullptr;
                }
            }
            if (!expect_op(p, OP_RBRACE, "}")) return nullptr;
            return e;
        }

        auto e = make_expr(EXPR_IDENT, line, col);
        e->sval = std::move(name); 
        return e;
    }

    if (check_op(p, OP_LPAREN)) {
        advance(p);
        ExprPtr e = parse_expr(p); // внутри скобок целое выражение, поэтому вызываем снова эту функцию
        if (!e) return nullptr;
        if (!expect_op(p, OP_RPAREN, ")")) return nullptr;
        return e;
    }

    if (check_op(p, OP_LBRACK)) { // массив
        advance(p);
        auto e = make_expr(EXPR_ARRAY, line, col);
        while (!p->had_error && !check_op(p, OP_RBRACK) && !check(p, TOKEN_EOF)) {
            ExprPtr elem = parse_expr(p); // какждый элемент массива это выражение
            if (!elem) return nullptr;
            e->elems.push_back(std::move(elem));
            if (!check_op(p, OP_RBRACK)) {
                if (!expect_op(p, OP_COMMA, ",")) return nullptr;
            }
        }
        if (!expect_op(p, OP_RBRACK, "]")) return nullptr;
        return e;
    }

    fail(p, "unexpected " + describe(cur(p)) + " in expression");
    return nullptr;
}

static bool parse_block(Parser *p, Block *out) { // после двоеточия 
    if (!expect(p, TOKEN_NEWLINE, "newline")) return false;
    if (!expect(p, TOKEN_INDENT, "indent")) return false;
    while (!p->had_error && !check(p, TOKEN_DEDENT) && !check(p, TOKEN_EOF)) {
        if (check(p, TOKEN_NEWLINE)) { advance(p); continue; }
        auto s = parse_stmt(p); // внутри блока
        if (!s) return false;
        out->push_back(std::move(s));
    }
    return expect(p, TOKEN_DEDENT, "dedent");
}

static std::unique_ptr<Stmt> make_stmt(StmtKind kind, int line, int col) {
    auto s = std::make_unique<Stmt>();
    s->kind = kind;
    s->line = line;
    s->col = col;
    s->is_mut = false;
    s->has_type = false;
    s->has_else = false;
    return s;
}

static std::unique_ptr<Stmt> parse_stmt(Parser *p) {
    int line = cur(p).line, col = cur(p).col;

    if (check_kw(p, KW_LET) || check_kw(p, KW_VAR)) {
        bool is_mut = check_kw(p, KW_VAR);
        advance(p);
        auto s = make_stmt(STMT_VAR, line, col);
        s->is_mut = is_mut;
        if (!expect_ident(p, &s->var_name)) return nullptr;
        if (check_op(p, OP_COLON)) {
            advance(p);
            s->has_type = true;
            if (!parse_type(p, &s->var_type)) return nullptr;
        }
        if (!expect_op(p, OP_EQ, "=")) return nullptr;
        s->init = parse_expr(p);
        if (!s->init) return nullptr;
        if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
        return s;
    }

    if (check_kw(p, KW_IF)) {
        advance(p);
        auto s = make_stmt(STMT_IF, line, col);
        s->cond = parse_expr(p);
        if (!s->cond) return nullptr;
        if (!expect_op(p, OP_COLON, ":")) return nullptr;
        if (!parse_block(p, &s->body)) return nullptr;
        if (check_kw(p, KW_ELSE)) {
            advance(p);
            if (!expect_op(p, OP_COLON, ":")) return nullptr;
            if (!parse_block(p, &s->else_body)) return nullptr;
            s->has_else = true;
        }
        return s;
    }

    if (check_kw(p, KW_WHILE)) {
        advance(p);
        auto s = make_stmt(STMT_WHILE, line, col);
        s->cond = parse_expr(p);
        if (!s->cond) return nullptr;
        if (!expect_op(p, OP_COLON, ":")) return nullptr;
        if (!parse_block(p, &s->body)) return nullptr;
        return s;
    }

    if (check_kw(p, KW_RETURN)) {
        advance(p);
        auto s = make_stmt(STMT_RETURN, line, col);
        if (!check(p, TOKEN_NEWLINE) && !check(p, TOKEN_EOF)) {
            s->ret_val = parse_expr(p);
            if (!s->ret_val) return nullptr;
        }
        if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
        return s;
    }

    if (check_kw(p, KW_BREAK)) {
        advance(p);
        if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
        return make_stmt(STMT_BREAK, line, col);
    }

    if (check_kw(p, KW_CONTINUE)) {
        advance(p);
        if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
        return make_stmt(STMT_CONTINUE, line, col);
    }

    ExprPtr lhs = parse_expr(p);
    if (!lhs) return nullptr;

    if (check_op(p, OP_EQ)) {
        if (!is_lvalue(*lhs)) {
            fail(p, "invalid assignment target");
            return nullptr;
        }
        advance(p);
        ExprPtr rhs = parse_expr(p);
        if (!rhs) return nullptr;
        if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
        auto s = make_stmt(STMT_ASSIGN, line, col);
        s->target = std::move(lhs);
        s->val = std::move(rhs);
        return s;
    }

    if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
    auto s = make_stmt(STMT_EXPR, line, col);
    s->expr = std::move(lhs);
    return s;
}

static std::unique_ptr<Decl> make_decl(DeclKind kind, int line, int col) {
    auto d = std::make_unique<Decl>();
    d->kind = kind;
    d->line = line;
    d->col = col;
    return d;
}

static std::unique_ptr<Decl> parse_decl(Parser *p) {
    int line = cur(p).line, col = cur(p).col;

    if (check_kw(p, KW_DEF)) {
        advance(p);
        auto d = make_decl(DECL_FUNC, line, col);
        if (!expect_ident(p, &d->name)) return nullptr;
        if (!expect_op(p, OP_LPAREN, "(")) return nullptr;
        while (!p->had_error && !check_op(p, OP_RPAREN) && !check(p, TOKEN_EOF)) {
            Param param;
            param.line = cur(p).line;
            param.col = cur(p).col;
            if (!expect_ident(p, &param.name)) return nullptr;
            if (!expect_op(p, OP_COLON, ":")) return nullptr;
            if (!parse_type(p, &param.type)) return nullptr;
            d->params.push_back(std::move(param));
            if (!check_op(p, OP_RPAREN)) {
                if (!expect_op(p, OP_COMMA, ",")) return nullptr;
            }
        }
        if (!expect_op(p, OP_RPAREN, ")")) return nullptr;
        if (!expect_op(p, OP_ARROW, "->")) return nullptr;
        if (!parse_type(p, &d->ret_type)) return nullptr;
        if (!expect_op(p, OP_COLON, ":")) return nullptr;
        if (!parse_block(p, &d->body)) return nullptr;
        return d;
    }

    if (check_kw(p, KW_STRUCT)) {
        advance(p);
        auto d = make_decl(DECL_STRUCT, line, col);
        if (!expect_ident(p, &d->name)) return nullptr;
        if (!expect_op(p, OP_COLON, ":")) return nullptr;
        if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
        if (!expect(p, TOKEN_INDENT, "indent")) return nullptr;
        while (!p->had_error && !check(p, TOKEN_DEDENT) && !check(p, TOKEN_EOF)) {
            if (check(p, TOKEN_NEWLINE)) { advance(p); continue; }
            FieldDecl fd;
            fd.line = cur(p).line;
            fd.col = cur(p).col;
            if (!expect_ident(p, &fd.name)) return nullptr;
            if (!expect_op(p, OP_COLON, ":")) return nullptr;
            if (!parse_type(p, &fd.type)) return nullptr;
            if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
            d->fields.push_back(std::move(fd));
        }
        if (!expect(p, TOKEN_DEDENT, "dedent")) return nullptr;
        return d;
    }

    if (check_kw(p, KW_TYPE)) {
        advance(p);
        auto d = make_decl(DECL_TYPE_ALIAS, line, col);
        if (!expect_ident(p, &d->name)) return nullptr;
        if (!expect_op(p, OP_EQ, "=")) return nullptr;
        if (!parse_type(p, &d->alias_type)) return nullptr;
        if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
        return d;
    }

    if (check_kw(p, KW_NAMESPACE)) {
        advance(p);
        auto d = make_decl(DECL_NAMESPACE, line, col);
        if (!expect_ident(p, &d->name)) return nullptr;
        if (!expect_op(p, OP_COLON, ":")) return nullptr;
        if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
        if (!expect(p, TOKEN_INDENT, "indent")) return nullptr;
        while (!p->had_error && !check(p, TOKEN_DEDENT) && !check(p, TOKEN_EOF)) {
            if (check(p, TOKEN_NEWLINE)) { advance(p); continue; }
            auto inner = parse_decl(p);
            if (!inner) return nullptr;
            d->decls.push_back(std::move(inner));
        }
        if (!expect(p, TOKEN_DEDENT, "dedent")) return nullptr;
        return d;
    }

    if (check_kw(p, KW_IMPL)) {
        advance(p);
        auto d = make_decl(DECL_IMPL, line, col);
        if (!expect_ident(p, &d->struct_name)) return nullptr;
        if (!expect_op(p, OP_COLON, ":")) return nullptr;
        if (!expect(p, TOKEN_NEWLINE, "newline")) return nullptr;
        if (!expect(p, TOKEN_INDENT, "indent")) return nullptr;
        while (!p->had_error && !check(p, TOKEN_DEDENT) && !check(p, TOKEN_EOF)) {
            if (check(p, TOKEN_NEWLINE)) { advance(p); continue; }
            if (!check_kw(p, KW_DEF)) {
                fail(p, "expected 'def' inside impl block");
                return nullptr;
            }
            auto method = parse_decl(p);
            if (!method) return nullptr;
            d->decls.push_back(std::move(method));
        }
        if (!expect(p, TOKEN_DEDENT, "dedent")) return nullptr;
        return d;
    }

    fail(p, "expected declaration (def / struct / type / namespace / impl), got " + describe(cur(p)));
    return nullptr;
}

bool parse(const std::vector<Token> *tokens, Program *out, ParseError *err) {
    Parser p = {tokens, 0, {}, false};
    while (!p.had_error && !check(&p, TOKEN_EOF)) {
        if (check(&p, TOKEN_NEWLINE)) { advance(&p); continue; }
        auto d = parse_decl(&p);
        if (!d) break;
        out->decls.push_back(std::move(d));
    }
    if (p.had_error) { *err = p.err; return false; }
    return true;
}

static std::string type_str(const Type &t) {
    switch (t.kind) {
    case TYPE_INT8:
        return "int8";
    case TYPE_INT16:
        return "int16";
    case TYPE_INT32:
        return "int32";
    case TYPE_INT64:
        return "int64";
    case TYPE_UINT8:
        return "uint8";
    case TYPE_UINT16:
        return "uint16";
    case TYPE_UINT32:
        return "uint32";
    case TYPE_UINT64:
        return "uint64";
    case TYPE_FLOAT32:
        return "float32";
    case TYPE_FLOAT64:
        return "float64";
    case TYPE_BOOL:
        return "bool";
    case TYPE_STRING:
        return "string";
    case TYPE_VOID:
        return "void";
    case TYPE_NAMED:
        return t.name;
    case TYPE_ARRAY:
        return "array[" + type_str(*t.elem) + ", " + std::to_string(t.array_size) + "]";
    }
    return "?";
}

static void dump_expr(const Expr &e, std::ostream &out) {
    static const char *BINOP_SYM[] = {
        "+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "and", "or"
    };
    switch (e.kind) {
    case EXPR_INT:
        out << e.ival;
        break;
    case EXPR_FLOAT:
        out << e.fval;
        break;
    case EXPR_BOOL:
        out << (e.bval ? "true" : "false");
        break;
    case EXPR_STRING:
        out << '"' << e.sval << '"';
        break;
    case EXPR_IDENT:
        out << e.sval;
        break;
    case EXPR_BINARY:
        out << "(";
        dump_expr(*e.lhs, out);
        out << " " << BINOP_SYM[e.binop] << " ";
        dump_expr(*e.rhs, out);
        out << ")";
        break;
    case EXPR_UNARY:
        out << (e.unop == UNOP_NEG ? "-" : "not ");
        dump_expr(*e.lhs, out);
        break;
    case EXPR_CAST:
        out << "(";
        dump_expr(*e.lhs, out);
        out << " as " << type_str(e.cast_type) << ")";
        break;
    case EXPR_INDEX:
        dump_expr(*e.lhs, out);
        out << "[";
        dump_expr(*e.rhs, out);
        out << "]";
        break;
    case EXPR_FIELD:
        dump_expr(*e.lhs, out);
        out << "." << e.sval;
        break;
    case EXPR_SCOPE:
        dump_expr(*e.lhs, out);
        out << "::" << e.sval;
        break;
    case EXPR_CALL:
        dump_expr(*e.lhs, out);
        out << "(";
        for (size_t i = 0; i < e.args.size(); i++) {
            if (i) out << ", ";
            dump_expr(*e.args[i], out);
        }
        out << ")";
        break;
    case EXPR_ARRAY:
        out << "[";
        for (size_t i = 0; i < e.elems.size(); i++) {
            if (i) out << ", ";
            dump_expr(*e.elems[i], out);
        }
        out << "]";
        break;
    case EXPR_STRUCT:
        out << e.sval << "{";
        for (size_t i = 0; i < e.fields.size(); i++) {
            if (i) out << ", ";
            out << e.fields[i].name << ": ";
            dump_expr(*e.fields[i].value, out);
        }
        out << "}";
        break;
    }
}

static void dump_block(const Block &b, std::ostream &out, int depth);

static void dump_stmt(const Stmt &s, std::ostream &out, int depth) {
    std::string pad(depth * 4, ' ');
    out << pad;
    switch (s.kind) {
    case STMT_VAR:
        out << (s.is_mut ? "var " : "let ") << s.var_name;
        if (s.has_type) out << ": " << type_str(s.var_type);
        out << " = ";
        dump_expr(*s.init, out);
        out << "\n";
        break;
    case STMT_ASSIGN:
        dump_expr(*s.target, out);
        out << " = ";
        dump_expr(*s.val, out);
        out << "\n";
        break;
    case STMT_IF:
        out << "if ";
        dump_expr(*s.cond, out);
        out << ":\n";
        dump_block(s.body, out, depth + 1);
        if (s.has_else) {
            out << pad << "else:\n";
            dump_block(s.else_body, out, depth + 1);
        }
        break;
    case STMT_WHILE:
        out << "while ";
        dump_expr(*s.cond, out);
        out << ":\n";
        dump_block(s.body, out, depth + 1);
        break;
    case STMT_RETURN:
        out << "return";
        if (s.ret_val) { out << " "; dump_expr(*s.ret_val, out); }
        out << "\n";
        break;
    case STMT_BREAK:
        out << "break\n";
        break;
    case STMT_CONTINUE:
        out << "continue\n";
        break;
    case STMT_EXPR:
        dump_expr(*s.expr, out);
        out << "\n";
        break;
    }
}

static void dump_block(const Block &b, std::ostream &out, int depth) {
    for (const auto &s : b) dump_stmt(*s, out, depth);
}

static void dump_decl(const Decl &d, std::ostream &out, int depth) {
    std::string pad(depth * 4, ' ');
    switch (d.kind) {
    case DECL_FUNC:
        out << pad << "def " << d.name << "(";
        for (size_t i = 0; i < d.params.size(); i++) {
            if (i) out << ", ";
            out << d.params[i].name << ": " << type_str(d.params[i].type);
        }
        out << ") -> " << type_str(d.ret_type) << ":\n";
        dump_block(d.body, out, depth + 1);
        out << "\n";
        break;
    case DECL_STRUCT:
        out << pad << "struct " << d.name << ":\n";
        for (const auto &f : d.fields)
            out << pad << "    " << f.name << ": " << type_str(f.type) << "\n";
        out << "\n";
        break;
    case DECL_TYPE_ALIAS:
        out << pad << "type " << d.name << " = " << type_str(d.alias_type) << "\n\n";
        break;
    case DECL_NAMESPACE:
        out << pad << "namespace " << d.name << ":\n";
        for (const auto &inner : d.decls) dump_decl(*inner, out, depth + 1);
        out << "\n";
        break;
    case DECL_IMPL:
        out << pad << "impl " << d.struct_name << ":\n";
        for (const auto &m : d.decls) dump_decl(*m, out, depth + 1);
        out << "\n";
        break;
    }
}

void dump_ast(const Program *prog, std::ostream &out) {
    for (const auto &d : prog->decls) dump_decl(*d, out, 0);
}

static void tree_node(std::ostream &out, const std::string &pfx, bool last,
                      const std::string &label, std::string *child_pfx) {
    out << pfx << (last ? "└── " : "├── ") << label << "\n";
    if (child_pfx) *child_pfx = pfx + (last ? "    " : "│   ");
}

static void tree_expr(const Expr &e, std::ostream &out, const std::string &pfx, bool last);

static void tree_expr_list(const std::vector<ExprPtr> &list,
                           std::ostream &out, const std::string &pfx) {
    for (size_t i = 0; i < list.size(); i++)
        tree_expr(*list[i], out, pfx, i + 1 == list.size());
}

static void tree_expr(const Expr &e, std::ostream &out, const std::string &pfx, bool last) {
    static const char *BINOP_SYM[] = {
        "+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "and", "or"
    };
    std::string cp;
    switch (e.kind) {
    case EXPR_INT:
        tree_node(out, pfx, last, "Int " + std::to_string(e.ival), nullptr);
        break;
    case EXPR_FLOAT:
        tree_node(out, pfx, last, "Float " + std::to_string(e.fval), nullptr);
        break;
    case EXPR_BOOL:
        tree_node(out, pfx, last, std::string("Bool ") + (e.bval ? "true" : "false"), nullptr);
        break;
    case EXPR_STRING:
        tree_node(out, pfx, last, "String \"" + e.sval + "\"", nullptr);
        break;
    case EXPR_IDENT:
        tree_node(out, pfx, last, "Ident " + e.sval, nullptr);
        break;
    case EXPR_BINARY:
        tree_node(out, pfx, last, std::string("Binary ") + BINOP_SYM[e.binop], &cp);
        tree_expr(*e.lhs, out, cp, false);
        tree_expr(*e.rhs, out, cp, true);
        break;
    case EXPR_UNARY:
        tree_node(out, pfx, last, e.unop == UNOP_NEG ? "Unary -" : "Unary not", &cp);
        tree_expr(*e.lhs, out, cp, true);
        break;
    case EXPR_CAST:
        tree_node(out, pfx, last, "Cast -> " + type_str(e.cast_type), &cp);
        tree_expr(*e.lhs, out, cp, true);
        break;
    case EXPR_INDEX:
        tree_node(out, pfx, last, "Index", &cp);
        tree_expr(*e.lhs, out, cp, false);
        tree_expr(*e.rhs, out, cp, true);
        break;
    case EXPR_FIELD:
        tree_node(out, pfx, last, "Field ." + e.sval, &cp);
        tree_expr(*e.lhs, out, cp, true);
        break;
    case EXPR_SCOPE:
        tree_node(out, pfx, last, "Scope ::" + e.sval, &cp);
        tree_expr(*e.lhs, out, cp, true);
        break;
    case EXPR_CALL:
        tree_node(out, pfx, last, "Call", &cp);
        tree_expr(*e.lhs, out, cp, e.args.empty());
        tree_expr_list(e.args, out, cp);
        break;
    case EXPR_ARRAY:
        tree_node(out, pfx, last, "Array [" + std::to_string(e.elems.size()) + " elems]", &cp);
        tree_expr_list(e.elems, out, cp);
        break;
    case EXPR_STRUCT:
        tree_node(out, pfx, last, "Struct " + e.sval, &cp);
        for (size_t i = 0; i < e.fields.size(); i++) {
            std::string fcp;
            tree_node(out, cp, i + 1 == e.fields.size(), "." + e.fields[i].name, &fcp);
            tree_expr(*e.fields[i].value, out, fcp, true);
        }
        break;
    }
}

static void tree_block(const Block &b, std::ostream &out, const std::string &pfx);

static void tree_stmt(const Stmt &s, std::ostream &out, const std::string &pfx, bool last) {
    std::string cp;
    switch (s.kind) {
    case STMT_VAR: {
        std::string label = (s.is_mut ? "Var " : "Let ") + s.var_name;
        if (s.has_type) label += ": " + type_str(s.var_type);
        tree_node(out, pfx, last, label, &cp);
        tree_expr(*s.init, out, cp, true);
        break;
    }
    case STMT_ASSIGN:
        tree_node(out, pfx, last, "Assign", &cp);
        tree_expr(*s.target, out, cp, false);
        tree_expr(*s.val, out, cp, true);
        break;
    case STMT_IF: {
        tree_node(out, pfx, last, "If", &cp);
        std::string cc, tc;
        tree_node(out, cp, !s.has_else, "cond", &cc);
        tree_expr(*s.cond, out, cc, true);
        tree_node(out, cp, !s.has_else, "then", &tc);
        tree_block(s.body, out, tc);
        if (s.has_else) {
            std::string ec;
            tree_node(out, cp, true, "else", &ec);
            tree_block(s.else_body, out, ec);
        }
        break;
    }
    case STMT_WHILE: {
        tree_node(out, pfx, last, "While", &cp);
        std::string cc, bc;
        tree_node(out, cp, false, "cond", &cc);
        tree_expr(*s.cond, out, cc, true);
        tree_node(out, cp, true, "body", &bc);
        tree_block(s.body, out, bc);
        break;
    }
    case STMT_RETURN:
        if (!s.ret_val) { tree_node(out, pfx, last, "Return", nullptr); break; }
        tree_node(out, pfx, last, "Return", &cp);
        tree_expr(*s.ret_val, out, cp, true);
        break;
    case STMT_BREAK:
        tree_node(out, pfx, last, "Break", nullptr);
        break;
    case STMT_CONTINUE:
        tree_node(out, pfx, last, "Continue", nullptr);
        break;
    case STMT_EXPR:
        tree_expr(*s.expr, out, pfx, last);
        break;
    }
}

static void tree_block(const Block &b, std::ostream &out, const std::string &pfx) {
    for (size_t i = 0; i < b.size(); i++)
        tree_stmt(*b[i], out, pfx, i + 1 == b.size());
}

static void tree_decl(const Decl &d, std::ostream &out, const std::string &pfx, bool last) {
    std::string cp;
    switch (d.kind) {
    case DECL_FUNC: {
        std::string label = "def " + d.name + "(";
        for (size_t i = 0; i < d.params.size(); i++) {
            if (i) label += ", ";
            label += d.params[i].name + ": " + type_str(d.params[i].type);
        }
        label += ") -> " + type_str(d.ret_type);
        tree_node(out, pfx, last, label, &cp);
        tree_block(d.body, out, cp);
        break;
    }
    case DECL_STRUCT:
        tree_node(out, pfx, last, "struct " + d.name, &cp);
        for (size_t i = 0; i < d.fields.size(); i++)
            tree_node(out, cp, i + 1 == d.fields.size(),
                      d.fields[i].name + ": " + type_str(d.fields[i].type), nullptr);
        break;
    case DECL_TYPE_ALIAS:
        tree_node(out, pfx, last, "type " + d.name + " = " + type_str(d.alias_type), nullptr);
        break;
    case DECL_NAMESPACE:
        tree_node(out, pfx, last, "namespace " + d.name, &cp);
        for (size_t i = 0; i < d.decls.size(); i++)
            tree_decl(*d.decls[i], out, cp, i + 1 == d.decls.size());
        break;
    case DECL_IMPL:
        tree_node(out, pfx, last, "impl " + d.struct_name, &cp);
        for (size_t i = 0; i < d.decls.size(); i++)
            tree_decl(*d.decls[i], out, cp, i + 1 == d.decls.size());
        break;
    }
}

void print_tree(const Program *prog, std::ostream &out) {
    out << "Program\n";
    for (size_t i = 0; i < prog->decls.size(); i++)
        tree_decl(*prog->decls[i], out, "", i + 1 == prog->decls.size());
}

#include "semantic.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

// копия типа 
static Type copy_type(const Type &t) {
    Type r;
    r.kind = t.kind;
    r.name = t.name;
    r.array_size = t.array_size;
    r.line = t.line;
    r.col = t.col;
    if (t.elem) r.elem = std::make_unique<Type>(copy_type(*t.elem));
    return r;
}

typedef enum { 
    SYM_VAR, 
    SYM_FUNC, 
    SYM_STRUCT, 
    SYM_TYPE,
    SYM_OVERLOADED // перегрузка
} SymKind;
 
struct SymInfo { // запись о символе
    SymKind kind; // тип
    bool is_mut; // мутабельность
    const Type *type; // указатели в дереве
    const std::vector<Param> *params; // парамтеры
    const std::vector<FieldDecl> *fields; // поля структуры
};

using Scope = std::map<std::string, SymInfo>; // имя и информация о символе

struct SemCtx { // глобальная структура
    std::vector<Scope> scopes; // стек вложенностей
    const Type *ret_type; // тип возврата
    bool in_loop; // внутри цикла?
    SemError err;
    bool had_error;
    std::map<std::string, std::vector<std::string>> overloads; // перегрузки
    std::map<std::string, int> overload_count; // счетчик перегрузок
};

// записывает первую ошибку, последующие игнорируются
static void fail(SemCtx *ctx, const std::string &msg, int line, int col) {
    if (ctx->had_error) return;
    ctx->err = {msg, line, col};
    ctx->had_error = true;
}

static void push_scope(SemCtx *ctx) { // добавить вложенность (функции/циклы/условия)
    ctx->scopes.emplace_back(); 

}
static void pop_scope(SemCtx *ctx) { // убрать вложенность
    ctx->scopes.pop_back(); 
}

// ищем с верхушки стека вниз для затенений
static SymInfo *lookup(SemCtx *ctx, const std::string &name) {
    for (int i = (int)ctx->scopes.size() - 1; i >= 0; i--) {
        auto it = ctx->scopes[i].find(name);
        if (it != ctx->scopes[i].end()) return &it->second;
    }
    return nullptr;
}

// добавляет символ уровень вложенности, ошибка если имя уже объявлено на этом уровне
static bool define(SemCtx *ctx, const std::string &name, SymInfo sym, int line, int col) {
    auto &scope = ctx->scopes.back();
    if (scope.count(name)) {
        fail(ctx, "'" + name + "' already declared in this scope", line, col);
        return false;
    }
    scope[name] = std::move(sym);
    return true;
}


// строка для сообщений об ошибках
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

// является ли тип числовым
static bool is_numeric(const Type &t) {
    switch (t.kind) {
    case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
    case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
    case TYPE_FLOAT32: case TYPE_FLOAT64:
        return true;
    default:
        return false;
    }
}

// является ли тип целочисленным (индексы массива)
static bool is_integer(const Type &t) {
    switch (t.kind) {
    case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
    case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
        return true;
    default:
        return false;
    }
}

static int numeric_rank(TypeKind k) {
    switch (k) {
    case TYPE_INT8: case TYPE_UINT8:
        return 1;
    case TYPE_INT16: case TYPE_UINT16:
        return 2;
    case TYPE_INT32: case TYPE_UINT32:
        return 3;
    case TYPE_INT64: case TYPE_UINT64:
        return 4;
    case TYPE_FLOAT32:
        return 5;
    case TYPE_FLOAT64:
        return 6;
    default:
        return 0;
    }
}

// тип результата бинарной числовой операции
static TypeKind common_numeric(TypeKind a, TypeKind b) {
    if (a == b) return a;
    bool af = (a == TYPE_FLOAT32 || a == TYPE_FLOAT64);
    bool bf = (b == TYPE_FLOAT32 || b == TYPE_FLOAT64);
    if (af || bf) return TYPE_FLOAT64;
    return numeric_rank(a) >= numeric_rank(b) ? a : b;
}

// неявное расширение
static bool can_widen(TypeKind from, TypeKind to) {
    if (from == to) return true;
    bool ff = (from == TYPE_FLOAT32 || from == TYPE_FLOAT64);
    bool tf = (to == TYPE_FLOAT32 || to == TYPE_FLOAT64);
    if (tf) return true;
    if (ff) return false;
    return numeric_rank(from) <= numeric_rank(to);
}

// ищем базовый тип псевдонима
static const Type *resolve(SemCtx *ctx, const Type &t) {
    if (t.kind != TYPE_NAMED) return &t;
    SymInfo *sym = lookup(ctx, t.name);
    if (!sym || sym->kind != SYM_TYPE) return &t;
    return resolve(ctx, *sym->type);
}

static bool types_equal(SemCtx *ctx, const Type &a, const Type &b) {
    const Type *ra = resolve(ctx, a);
    const Type *rb = resolve(ctx, b);
    if (ra->kind != rb->kind) return false;
    if (ra->kind == TYPE_NAMED) return ra->name == rb->name;
    if (ra->kind == TYPE_ARRAY)
        return ra->array_size == rb->array_size
            && types_equal(ctx, *ra->elem, *rb->elem);
    return true;
}

// заполняет *out примитивным типом (без name/elem/array_size)
static void set_primitive(Type *out, TypeKind kind) {
    out->kind = kind;
    out->name = "";
    out->array_size = 0;
    out->line = out->col = 0;
    out->elem.reset();
}

// A.1.13: размер типа в байтах для sizeof()
static std::optional<int64_t> type_size(SemCtx *ctx, const Type &t) {
    const Type *r = resolve(ctx, t);
    switch (r->kind) {
    case TYPE_INT8: case TYPE_UINT8: case TYPE_BOOL:
        return 1;
    case TYPE_INT16: case TYPE_UINT16:
        return 2;
    case TYPE_INT32: case TYPE_UINT32: case TYPE_FLOAT32:
        return 4;
    case TYPE_INT64: case TYPE_UINT64: case TYPE_FLOAT64:
        return 8;
    case TYPE_ARRAY: {
        auto elem = type_size(ctx, *r->elem);
        if (!elem) return std::nullopt;
        return *elem * r->array_size;
    }
    default:
        return std::nullopt;
    }
}


// вызывает check_expr для аргументов
// рекурсивно обходит выражение и выводит его тип в *out
static bool check_expr(SemCtx *ctx, const Expr *e, Type *out);

// проверяет вызов встроенной функции (print/input/exit/panic/len) и возвращает её тип
static bool check_builtin_call(SemCtx *ctx, const std::string &name, const std::vector<ExprPtr> &args, int line, int col, Type *out) {
    if (name == "print") {
        if (args.size() != 1) { 
            fail(ctx, "print() takes 1 argument", line, col); 
            return false; 
        }
        Type arg_t;
        if (!check_expr(ctx, args[0].get(), &arg_t)) return false;
        if (resolve(ctx, arg_t)->kind == TYPE_VOID) {
            fail(ctx, "print() cannot print void", line, col); 
            return false;
        }
        set_primitive(out, TYPE_VOID);
        return true;
    }
    if (name == "input") {
        if (!args.empty()) { fail(ctx, "input() takes no arguments", line, col); return false; }
        set_primitive(out, TYPE_STRING);
        return true;
    }
    if (name == "exit") {
        if (args.size() != 1) { 
            fail(ctx, "exit() takes 1 argument", line, col); 
            return false; 
        }
        Type arg_t;
        if (!check_expr(ctx, args[0].get(), &arg_t)) return false;
        Type int32_t_; set_primitive(&int32_t_, TYPE_INT32);
        if (!types_equal(ctx, arg_t, int32_t_)) {
            fail(ctx, "exit() argument must be int32, got " + type_str(arg_t), line, col);
            return false;
        }
        set_primitive(out, TYPE_VOID);
        return true;
    }
    if (name == "panic") {
        if (args.size() != 1) { fail(ctx, "panic() takes 1 argument", line, col); return false; }
        Type arg_t;
        if (!check_expr(ctx, args[0].get(), &arg_t)) return false;
        Type str_t; set_primitive(&str_t, TYPE_STRING);
        if (!types_equal(ctx, arg_t, str_t)) {
            fail(ctx, "panic() argument must be string, got " + type_str(arg_t), line, col);
            return false;
        }
        set_primitive(out, TYPE_VOID);
        return true;
    }
    if (name == "len") {
        if (args.size() != 1) { fail(ctx, "len() takes 1 argument", line, col); return false; }
        Type arg_t;
        if (!check_expr(ctx, args[0].get(), &arg_t)) return false;
        const Type *rt = resolve(ctx, arg_t);
        if (rt->kind != TYPE_ARRAY && rt->kind != TYPE_STRING) {
            fail(ctx, "len() requires array or string, got " + type_str(arg_t), line, col);
            return false;
        }
        set_primitive(out, TYPE_INT32);
        return true;
    }
    return false;
}

// является ли имя встроенной функцией
static bool is_builtin(const std::string &n) {
    return n == "print" || n == "input" || n == "exit" || n == "panic" || n == "len";
}

static bool check_expr(SemCtx *ctx, const Expr *e, Type *out) {
    switch (e->kind) {

    case EXPR_INT:
        set_primitive(out, TYPE_INT32);
        return true;
    case EXPR_FLOAT:
        set_primitive(out, TYPE_FLOAT64);
        return true;
    case EXPR_BOOL:
        set_primitive(out, TYPE_BOOL);
        return true;
    case EXPR_STRING:
        set_primitive(out, TYPE_STRING);
        return true;

    case EXPR_IDENT: {
        if (is_builtin(e->sval)) { 
            set_primitive(out, TYPE_VOID); 
            return true; 
        }
        SymInfo *sym = lookup(ctx, e->sval);
        if (!sym) {
            fail(ctx, "undefined identifier '" + e->sval + "'", e->line, e->col); return false;
        }
        if (sym->kind != SYM_VAR) {
            fail(ctx, "'" + e->sval + "' is not a variable", e->line, e->col); return false;
        }
        *out = copy_type(*sym->type);
        return true;
    }

    case EXPR_BINARY: {
        Type lt, rt;
        if (!check_expr(ctx, e->lhs.get(), &lt)) return false;
        if (!check_expr(ctx, e->rhs.get(), &rt)) return false;

        if (e->binop == BINOP_AND || e->binop == BINOP_OR) {
            Type bool_t; set_primitive(&bool_t, TYPE_BOOL);
            if (!types_equal(ctx, lt, bool_t) || !types_equal(ctx, rt, bool_t)) {
                fail(ctx, "'and'/'or' require bool operands", e->line, e->col); 
                return false;
            }
            set_primitive(out, TYPE_BOOL);
            return true;
        }

        const Type *rlt = resolve(ctx, lt);
        const Type *rrt = resolve(ctx, rt);

        bool lstr = (rlt->kind == TYPE_STRING), rstr = (rrt->kind == TYPE_STRING);
        if (lstr || rstr) {
            if (e->binop == BINOP_ADD) {
                if (!lstr || !rstr) {
                    fail(ctx, "string concatenation requires both operands to be string", e->line, e->col);
                    return false;
                }
                set_primitive(out, TYPE_STRING); 
                return true;
            }
            if (e->binop == BINOP_EQ || e->binop == BINOP_NEQ) {
                if (!lstr || !rstr) {
                    fail(ctx, "type mismatch: " + type_str(lt) + " and " + type_str(rt), e->line, e->col);
                    return false;
                }
                set_primitive(out, TYPE_BOOL); 
                return true;
            }
            fail(ctx, "operator not supported for string", e->line, e->col);
            return false;
        }

        if (!is_numeric(lt) || !is_numeric(rt)) {
            if (!types_equal(ctx, lt, rt)) {
                fail(ctx, "type mismatch: " + type_str(lt) + " and " + type_str(rt), e->line, e->col);
                return false;
            }
        }

        TypeKind common = common_numeric(rlt->kind, rrt->kind);

        switch (e->binop) {
        case BINOP_ADD: case BINOP_SUB: case BINOP_MUL: case BINOP_DIV: case BINOP_MOD:
            if (!is_numeric(lt) || !is_numeric(rt)) {
                fail(ctx, "arithmetic requires numeric types", e->line, e->col); 
                return false;
            }
            set_primitive(out, common); return true;
        case BINOP_EQ: case BINOP_NEQ:
            if (!is_numeric(lt) && rlt->kind != TYPE_BOOL) {
                fail(ctx, "'=='/'!=' require numeric or bool", e->line, e->col); 
                return false;
            }
            set_primitive(out, TYPE_BOOL); return true;
        case BINOP_LT: case BINOP_GT: case BINOP_LEQ: case BINOP_GEQ:
            if (!is_numeric(lt) || !is_numeric(rt)) {
                fail(ctx, "comparison requires numeric types", e->line, e->col); 
                return false;
            }
            set_primitive(out, TYPE_BOOL); return true;
        default: break;
        }
        return false;
    }

    case EXPR_UNARY: {
        Type ot;
        if (!check_expr(ctx, e->lhs.get(), &ot)) return false;
        if (e->unop == UNOP_NOT) {
            Type bool_t; set_primitive(&bool_t, TYPE_BOOL);
            if (!types_equal(ctx, ot, bool_t)) {
                fail(ctx, "'not' requires bool, got " + type_str(ot), e->line, e->col); return false;
            }
            set_primitive(out, TYPE_BOOL);
        } else {
            if (!is_numeric(ot)) {
                fail(ctx, "unary '-' requires numeric type, got " + type_str(ot), e->line, e->col);
                return false;
            }
            *out = copy_type(ot);
        }
        return true;
    }

    case EXPR_CAST: {
        Type ot;
        if (!check_expr(ctx, e->lhs.get(), &ot)) return false;
        const Type *src = resolve(ctx, ot);
        const Type *dst = resolve(ctx, e->cast_type);
        if (!is_numeric(*src) && src->kind != TYPE_BOOL) {
            fail(ctx, "cannot cast from " + type_str(ot), e->line, e->col); return false;
        }
        if (!is_numeric(*dst) && dst->kind != TYPE_BOOL) {
            fail(ctx, "cannot cast to " + type_str(e->cast_type), e->line, e->col); return false;
        }
        *out = copy_type(e->cast_type);
        return true;
    }

    // A.1.13: sizeof(type) -- известен на этапе компиляции, сворачиваем в EXPR_INT.
    case EXPR_SIZEOF: {
        auto sz = type_size(ctx, e->cast_type);
        if (!sz) {
            fail(ctx, "sizeof: unsupported type '" + type_str(e->cast_type) + "'", e->line, e->col);
            return false;
        }
        Expr *m = const_cast<Expr *>(e);
        m->kind = EXPR_INT;
        m->ival = *sz;
        set_primitive(out, TYPE_INT32);
        return true;
    }

    // A.1.13: typeof(expr) -- имя статического типа как строковый литерал;
    case EXPR_TYPEOF: {
        Type lt;
        if (!check_expr(ctx, e->lhs.get(), &lt)) return false;
        Expr *m = const_cast<Expr *>(e);
        m->kind = EXPR_STRING;
        m->sval = type_str(lt);
        set_primitive(out, TYPE_STRING);
        return true;
    }

    case EXPR_INDEX: {
        Type base_t, idx_t;
        if (!check_expr(ctx, e->lhs.get(), &base_t)) return false;
        if (!check_expr(ctx, e->rhs.get(), &idx_t)) return false;
        const Type *rb = resolve(ctx, base_t);
        if (rb->kind != TYPE_ARRAY) {
            fail(ctx, "index requires array, got " + type_str(base_t), e->line, e->col); return false;
        }
        if (!is_integer(idx_t)) {
            fail(ctx, "array index must be integer, got " + type_str(idx_t), e->line, e->col);
            return false;
        }
        *out = copy_type(*rb->elem);
        return true;
    }

    case EXPR_FIELD: {
        Type base_t;
        if (!check_expr(ctx, e->lhs.get(), &base_t)) return false;
        const Type *rb = resolve(ctx, base_t);
        if (rb->kind != TYPE_NAMED) {
            fail(ctx, "field access on non-struct type " + type_str(base_t), e->line, e->col);
            return false;
        }
        SymInfo *sym = lookup(ctx, rb->name);
        if (!sym || sym->kind != SYM_STRUCT) {
            fail(ctx, "'" + rb->name + "' is not a struct", e->line, e->col); return false;
        }
        for (const auto &f : *sym->fields) {
            if (f.name == e->sval) { *out = copy_type(f.type); return true; }
        }
        fail(ctx, "struct '" + rb->name + "' has no field '" + e->sval + "'", e->line, e->col);
        return false;
    }

    case EXPR_SCOPE: {
        set_primitive(out, TYPE_VOID);
        return true;
    }

    case EXPR_CALL: {
        if (e->lhs->kind == EXPR_IDENT && is_builtin(e->lhs->sval))
            return check_builtin_call(ctx, e->lhs->sval, e->args, e->line, e->col, out);

        std::string fname;
        std::vector<Type> extra_args;

        if (e->lhs->kind == EXPR_IDENT) {
            fname = e->lhs->sval;
        } else if (e->lhs->kind == EXPR_SCOPE && e->lhs->lhs->kind == EXPR_IDENT) {
            fname = e->lhs->lhs->sval + "::" + e->lhs->sval;
        } else if (e->lhs->kind == EXPR_FIELD) {
            Type base_t;
            if (!check_expr(ctx, e->lhs->lhs.get(), &base_t)) return false;
            const Type *rb = resolve(ctx, base_t);
            if (rb->kind == TYPE_NAMED) {
                std::string method_name = rb->name + "::" + e->lhs->sval;
                SymInfo *msym = lookup(ctx, method_name);
                if (msym && msym->kind == SYM_FUNC) {
                    fname = method_name;
                    extra_args.push_back(copy_type(base_t)); // self передаётся первым аргументом
                    goto found_fname;
                }
            }
            fail(ctx, "invalid call expression", e->line, e->col); return false;
        } else {
            fail(ctx, "invalid call expression", e->line, e->col); return false;
        }
        found_fname:;

        SymInfo *sym = lookup(ctx, fname);
        if (!sym) {
            fail(ctx, "undefined function '" + fname + "'", e->line, e->col); return false;
        }

        // A.2.8 / A.3.1: у имени несколько версий — ищем первую, чьи параметры подходят
        // под типы аргументов (точное совпадение или неявное расширение, см. can_widen)
        if (sym->kind == SYM_OVERLOADED) {
            auto &versions = ctx->overloads[fname];
            std::vector<Type> arg_types;
            for (auto &a : extra_args) arg_types.push_back(copy_type(a));
            for (const auto &ea : e->args) {
                Type t;
                if (!check_expr(ctx, ea.get(), &t)) return false;
                arg_types.push_back(std::move(t));
            }
            for (const auto &vname : versions) {
                SymInfo *vsym = lookup(ctx, vname);
                if (!vsym || vsym->kind != SYM_FUNC) continue;
                if (vsym->params->size() != arg_types.size()) continue;
                bool ok = true;
                for (size_t i = 0; i < arg_types.size(); i++) {
                    const Type &pt = (*vsym->params)[i].type;
                    const Type &at = arg_types[i];
                    if (!types_equal(ctx, pt, at)) {
                        // A.3.1: точные типы не совпали — допускаем неявное расширение аргумента
                        const Type *rp = resolve(ctx, pt), *ra = resolve(ctx, at);
                        if (!is_numeric(*rp) || !is_numeric(*ra) || !can_widen(ra->kind, rp->kind))
                            { ok = false; break; }
                    }
                }
                if (ok) { *out = copy_type(*vsym->type); return true; }
            }
            fail(ctx, "no matching overload for '" + fname + "'", e->line, e->col);
            return false;
        }

        if (sym->kind != SYM_FUNC) {
            fail(ctx, "'" + fname + "' is not a function", e->line, e->col); return false;
        }

        size_t total_args = extra_args.size() + e->args.size();
        if (total_args != sym->params->size()) {
            fail(ctx, "'" + fname + "' expects " + std::to_string(sym->params->size())
                 + " arguments, got " + std::to_string(total_args), e->line, e->col);
            return false;
        }

        for (size_t i = 0; i < extra_args.size(); i++) {
            if (!types_equal(ctx, extra_args[i], (*sym->params)[i].type)) {
                const Type *rp = resolve(ctx, (*sym->params)[i].type);
                const Type *ra = resolve(ctx, extra_args[i]);
                if (!is_numeric(*rp) || !is_numeric(*ra) || !can_widen(ra->kind, rp->kind)) {
                    fail(ctx, "argument 1 type mismatch", e->line, e->col);
                    return false;
                }
            }
        }

        for (size_t i = 0; i < e->args.size(); i++) {
            size_t pi = i + extra_args.size();
            Type arg_t;
            if (!check_expr(ctx, e->args[i].get(), &arg_t)) return false;
            if (!types_equal(ctx, arg_t, (*sym->params)[pi].type)) {
                const Type *rp = resolve(ctx, (*sym->params)[pi].type);
                const Type *ra = resolve(ctx, arg_t);
                if (!is_numeric(*rp) || !is_numeric(*ra) || !can_widen(ra->kind, rp->kind)) {
                    fail(ctx, "argument " + std::to_string(pi + 1) + " of '" + fname
                         + "': expected " + type_str((*sym->params)[pi].type)
                         + ", got " + type_str(arg_t), e->args[i]->line, e->args[i]->col);
                    return false;
                }
            }
        }
        *out = copy_type(*sym->type);
        return true;
    }

    case EXPR_ARRAY: {
        if (e->elems.empty()) {
            fail(ctx, "cannot infer type of empty array literal", e->line, e->col); return false;
        }
        Type elem_t;
        if (!check_expr(ctx, e->elems[0].get(), &elem_t)) return false;
        for (size_t i = 1; i < e->elems.size(); i++) {
            Type t;
            if (!check_expr(ctx, e->elems[i].get(), &t)) return false;
            if (!types_equal(ctx, t, elem_t)) {
                fail(ctx, "array elements must have the same type",
                     e->elems[i]->line, e->elems[i]->col);
                return false;
            }
        }
        out->kind = TYPE_ARRAY;
        out->array_size = (int)e->elems.size();
        out->name = "";
        out->line = out->col = 0;
        out->elem = std::make_unique<Type>(copy_type(elem_t));
        return true;
    }

    case EXPR_STRUCT: {
        SymInfo *sym = lookup(ctx, e->sval);
        if (!sym || sym->kind != SYM_STRUCT) {
            fail(ctx, "'" + e->sval + "' is not a struct", e->line, e->col); return false;
        }
        if (e->fields.size() != sym->fields->size()) {
            fail(ctx, "struct '" + e->sval + "' has " + std::to_string(sym->fields->size())
                 + " fields, got " + std::to_string(e->fields.size()), e->line, e->col);
            return false;
        }
        for (const auto &fd : *sym->fields) {
            bool found = false;
            for (const auto &fi : e->fields) {
                if (fi.name != fd.name) continue;
                found = true;
                Type ft;
                if (!check_expr(ctx, fi.value.get(), &ft)) return false;
                if (!types_equal(ctx, ft, fd.type)) {
                    fail(ctx, "field '" + fd.name + "': expected " + type_str(fd.type)
                         + ", got " + type_str(ft), fi.value->line, fi.value->col);
                    return false;
                }
                break;
            }
            if (!found) {
                fail(ctx, "struct '" + e->sval + "' missing field '" + fd.name + "'",
                     e->line, e->col);
                return false;
            }
        }
        out->kind = TYPE_NAMED;
        out->name = e->sval;
        out->array_size = 0;
        out->line = out->col = 0;
        out->elem.reset();
        return true;
    }

    }
    return false;
}


// проверяет все стейтменты блока по порядку
static bool check_block(SemCtx *ctx, const Block &b);

// можно ли присвоить в это выражение (только var-переменные и их поля/индексы)
static bool is_assignable(SemCtx *ctx, const Expr *e) {
    if (e->kind == EXPR_IDENT) {
        SymInfo *sym = lookup(ctx, e->sval);
        return sym && sym->kind == SYM_VAR && sym->is_mut;
    }
    if (e->kind == EXPR_FIELD || e->kind == EXPR_INDEX)
        return is_assignable(ctx, e->lhs.get());
    return false;
}

// проверяет один стейтмент: типы, mutability, контекст (loop, return)
static bool check_stmt(SemCtx *ctx, const Stmt *s) {
    switch (s->kind) {

    case STMT_VAR: {
        Type init_t;
        if (!check_expr(ctx, s->init.get(), &init_t)) return false;

        if (s->has_type && !types_equal(ctx, s->var_type, init_t)) {
            const Type *rv = resolve(ctx, s->var_type);
            const Type *ri = resolve(ctx, init_t);
            if (!is_numeric(*rv) || !is_numeric(*ri) || !can_widen(ri->kind, rv->kind)) {
                fail(ctx, "'" + s->var_name + "': declared as " + type_str(s->var_type)
                     + " but initialized with " + type_str(init_t), s->line, s->col);
                return false;
            }
        }
        const Type &var_type = s->has_type ? s->var_type : init_t;
        if (resolve(ctx, var_type)->kind == TYPE_VOID) {
            fail(ctx, "variable '" + s->var_name + "' cannot have type void", s->line, s->col);
            return false;
        }
        SymInfo sym;
        sym.kind = SYM_VAR;
        sym.is_mut = s->is_mut;
        sym.params = nullptr;
        sym.fields = nullptr;
        sym.type = s->has_type ? &s->var_type : nullptr;
        if (!define(ctx, s->var_name, std::move(sym), s->line, s->col)) return false;
        // A.1.7: тип не указан явно (let x = 42) — берём тип из инициализатора.
        if (!s->has_type) {
            ctx->scopes.back()[s->var_name].type = new Type(copy_type(init_t));
        }
        return true;
    }

    case STMT_ASSIGN: {
        if (!is_assignable(ctx, s->target.get())) {
            fail(ctx, "cannot assign to immutable variable", s->line, s->col); return false;
        }
        Type ltype, rtype;
        if (!check_expr(ctx, s->target.get(), &ltype)) return false;
        if (!check_expr(ctx, s->val.get(), &rtype)) return false;
        if (!types_equal(ctx, ltype, rtype)) {
            const Type *rl = resolve(ctx, ltype), *rr = resolve(ctx, rtype);
            if (!is_numeric(*rl) || !is_numeric(*rr) || !can_widen(rr->kind, rl->kind)) {
                fail(ctx, "assignment type mismatch: " + type_str(ltype)
                     + " = " + type_str(rtype), s->line, s->col);
                return false;
            }
        }
        return true;
    }

    case STMT_IF: {
        Type cond_t;
        if (!check_expr(ctx, s->cond.get(), &cond_t)) return false;
        Type bool_t; set_primitive(&bool_t, TYPE_BOOL);
        if (!types_equal(ctx, cond_t, bool_t)) {
            fail(ctx, "if condition must be bool, got " + type_str(cond_t), s->line, s->col);
            return false;
        }
        push_scope(ctx);
        bool ok = check_block(ctx, s->body);
        pop_scope(ctx);
        if (!ok) return false;
        if (s->has_else) {
            push_scope(ctx);
            ok = check_block(ctx, s->else_body);
            pop_scope(ctx);
        }
        return ok;
    }

    case STMT_WHILE: {
        Type cond_t;
        if (!check_expr(ctx, s->cond.get(), &cond_t)) return false;
        Type bool_t; set_primitive(&bool_t, TYPE_BOOL);
        if (!types_equal(ctx, cond_t, bool_t)) {
            fail(ctx, "while condition must be bool, got " + type_str(cond_t), s->line, s->col);
            return false;
        }
        bool prev_loop = ctx->in_loop;
        ctx->in_loop = true;
        push_scope(ctx);
        bool ok = check_block(ctx, s->body);
        pop_scope(ctx);
        ctx->in_loop = prev_loop;
        return ok;
    }

    case STMT_RETURN: {
        if (!s->ret_val) {
            if (ctx->ret_type->kind != TYPE_VOID) {
                fail(ctx, "non-void function must return a value", s->line, s->col); return false;
            }
            return true;
        }
        Type rt;
        if (!check_expr(ctx, s->ret_val.get(), &rt)) return false;
        if (!types_equal(ctx, rt, *ctx->ret_type)) {
            fail(ctx, "return type mismatch: expected " + type_str(*ctx->ret_type)
                 + ", got " + type_str(rt), s->line, s->col);
            return false;
        }
        return true;
    }

    case STMT_BREAK: case STMT_CONTINUE:
        if (!ctx->in_loop) {
            fail(ctx, s->kind == STMT_BREAK ? "break outside loop" : "continue outside loop",
                 s->line, s->col);
            return false;
        }
        return true;

    case STMT_EXPR: {
        Type t;
        return check_expr(ctx, s->expr.get(), &t);
    }

    }
    return true;
}

static bool check_block(SemCtx *ctx, const Block &b) {
    for (const auto &s : b)
        if (!check_stmt(ctx, s.get())) return false;
    return true;
}


// первый проход: регистрирует имена объявлений без проверки тел
static bool collect_decl(SemCtx *ctx, const Decl *d, const std::string &ns_prefix = "");

// просто обходит список объявлений — верхний уровень, namespace или impl
static bool collect_decls(SemCtx *ctx, const DeclList &decls, const std::string &ns_prefix = "") {
    for (const auto &d : decls)
        if (!collect_decl(ctx, d.get(), ns_prefix)) return false;
    return true;
}

static bool collect_decl(SemCtx *ctx, const Decl *d, const std::string &ns_prefix) {
    std::string full_name = ns_prefix.empty() ? d->name : ns_prefix + "::" + d->name;

    switch (d->kind) {
    case DECL_FUNC: {
        SymInfo sym;
        sym.kind = SYM_FUNC;
        sym.is_mut = false;
        sym.type = &d->ret_type;
        sym.params = &d->params;
        sym.fields = nullptr;
        SymInfo *existing = lookup(ctx, full_name);
        if (existing && (existing->kind == SYM_FUNC || existing->kind == SYM_OVERLOADED)) {
            // A.2.8: вторая функция с тем же именем — превращаем в перегрузку.
            int idx = ctx->overload_count[full_name]++;
            std::string versioned = full_name + "$" + std::to_string(idx);
            if (idx == 0) {
                SymInfo orig = *existing;
                define(ctx, full_name + "$orig", std::move(orig), d->line, d->col);
                ctx->overloads[full_name].push_back(full_name + "$orig");
                ctx->scopes.back()[full_name].kind = SYM_OVERLOADED;
            }
            ctx->overloads[full_name].push_back(versioned);
            return define(ctx, versioned, std::move(sym), d->line, d->col);
        }
        return define(ctx, full_name, std::move(sym), d->line, d->col);
    }
    case DECL_STRUCT: {
        SymInfo sym;
        sym.kind = SYM_STRUCT;
        sym.is_mut = false;
        sym.type = nullptr;
        sym.params = nullptr;
        sym.fields = &d->fields;
        return define(ctx, full_name, std::move(sym), d->line, d->col);
    }

    case DECL_TYPE_ALIAS: {
        SymInfo sym;
        sym.kind = SYM_TYPE;
        sym.is_mut = false;
        sym.type = &d->alias_type;
        sym.params = nullptr;
        sym.fields = nullptr;
        return define(ctx, full_name, std::move(sym), d->line, d->col);
    }
    // namespace ничего не определяет сам — только добавляет префикс Name:: вложенным объявлениям
    case DECL_NAMESPACE: {
        std::string new_prefix = ns_prefix.empty() ? d->name : ns_prefix + "::" + d->name;
        return collect_decls(ctx, d->decls, new_prefix);
    }
    // A.2.12: impl StructName: — методы регистрируются как обычные функции
    case DECL_IMPL: {
        std::string impl_prefix = d->struct_name;
        return collect_decls(ctx, d->decls, impl_prefix);
    }
    }
    return true;
}

// второй проход: проверяет тело функции, зная все объявления из первого прохода
static bool check_decl(SemCtx *ctx, const Decl *d) {
    if (d->kind == DECL_FUNC) {
        static const Type void_type = []{ Type t; t.kind = TYPE_VOID; t.array_size = 0; t.line = t.col = 0; return t; }();
        ctx->ret_type = &d->ret_type;
        ctx->in_loop = false;

        push_scope(ctx);
        // параметры функции (включая self у методов) — обычные неизменяемые переменные внутри тела
        for (const auto &p : d->params) {
            SymInfo sym;
            sym.kind = SYM_VAR;
            sym.is_mut = false;
            sym.type = &p.type;
            sym.params = nullptr;
            sym.fields = nullptr;
            if (!define(ctx, p.name, std::move(sym), p.line, p.col)) {
                pop_scope(ctx); return false;
            }
        }
        bool ok = check_block(ctx, d->body);
        pop_scope(ctx);
        return ok;
    }
    // namespace сам не проверяется — спускаемся во вложенные объявления
    if (d->kind == DECL_NAMESPACE) {
        for (const auto &inner : d->decls)
            if (!check_decl(ctx, inner.get())) return false;
    }
    // A.2.12: тело каждого метода проверяется как обычная функция
    if (d->kind == DECL_IMPL) {
        for (const auto &method : d->decls)
            if (!check_decl(ctx, method.get())) return false;
    }
    return true;
}


bool sem_check(const Program *prog, SemError *err) {
    static const Type void_type = []{ Type t; t.kind = TYPE_VOID; t.array_size = 0; t.line = t.col = 0; return t; }();

    SemCtx ctx;
    ctx.in_loop = false;
    ctx.had_error = false;
    ctx.ret_type = &void_type;

    push_scope(&ctx);

    // два прохода: сначала регистрируем все объявления, потом проверяем тела.
    // это позволяет функциям вызывать друг друга вне зависимости от порядка объявления.
    if (!collect_decls(&ctx, prog->decls)) { *err = ctx.err; return false; }
    for (const auto &d : prog->decls)
        if (!check_decl(&ctx, d.get())) { *err = ctx.err; return false; }

    pop_scope(&ctx);
    return true;
}

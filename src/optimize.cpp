#include "optimize.hpp"

// B.2.4: проход оптимизаций над AST после семантической проверки.

static ExprPtr optimize_expr(ExprPtr e);
static Block optimize_block(Block &block);

static ExprPtr make_int(int64_t val, int line, int col) {
    auto e = std::make_unique<Expr>();
    e->kind = EXPR_INT;
    e->ival = val;
    e->line = line; e->col = col;
    return e;
}

static ExprPtr make_float(double val, int line, int col) {
    auto e = std::make_unique<Expr>();
    e->kind = EXPR_FLOAT;
    e->fval = val;
    e->line = line; e->col = col;
    return e;
}

static ExprPtr make_bool(bool val, int line, int col) {
    auto e = std::make_unique<Expr>();
    e->kind = EXPR_BOOL;
    e->bval = val;
    e->line = line; e->col = col;
    return e;
}

static ExprPtr make_string(std::string val, int line, int col) {
    auto e = std::make_unique<Expr>();
    e->kind = EXPR_STRING;
    e->sval = std::move(val);
    e->line = line; e->col = col;
    return e;
}

// Выражение с побочными эффектами (вызов функции) нельзя устранять
static bool has_side_effects(const Expr &e) {
    if (e.kind == EXPR_CALL)
        return true;
    if (e.lhs && has_side_effects(*e.lhs)) return true;
    if (e.rhs && has_side_effects(*e.rhs)) return true;
    for (const auto &a : e.args)   if (has_side_effects(*a)) return true;
    for (const auto &el : e.elems) if (has_side_effects(*el)) return true;
    for (const auto &f : e.fields) if (has_side_effects(*f.value)) return true;
    return false;
}

static bool is_int_const(const Expr &e, int64_t want) {
    return e.kind == EXPR_INT && e.ival == want;
}

static ExprPtr fold_binary(ExprPtr e) {
    Expr &l = *e->lhs;
    Expr &r = *e->rhs;
    int line = e->line, col = e->col;

    if (l.kind == EXPR_STRING && r.kind == EXPR_STRING) {
        switch (e->binop) {
        case BINOP_ADD: return make_string(l.sval + r.sval, line, col);
        case BINOP_EQ:  return make_bool(l.sval == r.sval, line, col);
        case BINOP_NEQ: return make_bool(l.sval != r.sval, line, col);
        default: break;
        }
    }

    // Логические операции и сравнения
    if (l.kind == EXPR_BOOL && r.kind == EXPR_BOOL) {
        switch (e->binop) {
        case BINOP_AND: return make_bool(l.bval && r.bval, line, col);
        case BINOP_OR:  return make_bool(l.bval || r.bval, line, col);
        case BINOP_EQ:  return make_bool(l.bval == r.bval, line, col);
        case BINOP_NEQ: return make_bool(l.bval != r.bval, line, col);
        default: break;
        }
    }

    // Арифметика и сравнения над числовыми литералами.
    bool l_num = (l.kind == EXPR_INT || l.kind == EXPR_FLOAT);
    bool r_num = (r.kind == EXPR_INT || r.kind == EXPR_FLOAT);
    if (l_num && r_num) {
        bool is_float = (l.kind == EXPR_FLOAT || r.kind == EXPR_FLOAT);
        if (!is_float) {
            int64_t a = l.ival, b = r.ival;
            switch (e->binop) {
            case BINOP_ADD: return make_int(a + b, line, col);
            case BINOP_SUB: return make_int(a - b, line, col);
            case BINOP_MUL: return make_int(a * b, line, col);
            case BINOP_DIV: if (b != 0) return make_int(a / b, line, col); break;
            case BINOP_MOD: if (b != 0) return make_int(a % b, line, col); break;
            case BINOP_EQ:  return make_bool(a == b, line, col);
            case BINOP_NEQ: return make_bool(a != b, line, col);
            case BINOP_LT:  return make_bool(a < b, line, col);
            case BINOP_GT:  return make_bool(a > b, line, col);
            case BINOP_LEQ: return make_bool(a <= b, line, col);
            case BINOP_GEQ: return make_bool(a >= b, line, col);
            default: break;
            }
        } else {
            double a = (l.kind == EXPR_FLOAT) ? l.fval : (double)l.ival;
            double b = (r.kind == EXPR_FLOAT) ? r.fval : (double)r.ival;
            switch (e->binop) {
            case BINOP_ADD: return make_float(a + b, line, col);
            case BINOP_SUB: return make_float(a - b, line, col);
            case BINOP_MUL: return make_float(a * b, line, col);
            case BINOP_DIV: if (b != 0.0) return make_float(a / b, line, col); break;
            case BINOP_EQ:  return make_bool(a == b, line, col);
            case BINOP_NEQ: return make_bool(a != b, line, col);
            case BINOP_LT:  return make_bool(a < b, line, col);
            case BINOP_GT:  return make_bool(a > b, line, col);
            case BINOP_LEQ: return make_bool(a <= b, line, col);
            case BINOP_GEQ: return make_bool(a >= b, line, col);
            default: break;
            }
        }
    }

    // убираем 0/1
    switch (e->binop) {
    case BINOP_ADD:
        if (is_int_const(r, 0)) return std::move(e->lhs); // x + 0 -> x
        if (is_int_const(l, 0)) return std::move(e->rhs); // 0 + x -> x
        break;
    case BINOP_SUB:
        if (is_int_const(r, 0)) return std::move(e->lhs); // x-0=x
        break;
    case BINOP_MUL:
        if (is_int_const(r, 1)) return std::move(e->lhs); // x*1=x
        if (is_int_const(l, 1)) return std::move(e->rhs); // 1*x=x
        if (is_int_const(r, 0) && !has_side_effects(l)) return std::move(e->rhs); // x*0=0
        if (is_int_const(l, 0) && !has_side_effects(r)) return std::move(e->lhs); // 0*x=0
        break;
    case BINOP_DIV:
        if (is_int_const(r, 1)) return std::move(e->lhs); // x/1=x
        break;
    default: break;
    }

    return e;
}

static ExprPtr fold_unary(ExprPtr e) {
    Expr &v = *e->lhs;
    switch (e->unop) {
    case UNOP_NEG:
        if (v.kind == EXPR_INT)   return make_int(-v.ival, e->line, e->col);
        if (v.kind == EXPR_FLOAT) return make_float(-v.fval, e->line, e->col);
        break;
    case UNOP_NOT:
        if (v.kind == EXPR_BOOL) return make_bool(!v.bval, e->line, e->col);
        break;
    }
    return e;
}

static ExprPtr optimize_expr(ExprPtr e) {
    if (!e) return e;
    switch (e->kind) {
    case EXPR_BINARY:
        e->lhs = optimize_expr(std::move(e->lhs));
        e->rhs = optimize_expr(std::move(e->rhs));
        return fold_binary(std::move(e));
    case EXPR_UNARY:
        e->lhs = optimize_expr(std::move(e->lhs));
        return fold_unary(std::move(e));
    case EXPR_CAST:
        e->lhs = optimize_expr(std::move(e->lhs));
        return e;
    case EXPR_INDEX:
        e->lhs = optimize_expr(std::move(e->lhs));
        e->rhs = optimize_expr(std::move(e->rhs));
        return e;
    case EXPR_FIELD:
        e->lhs = optimize_expr(std::move(e->lhs));
        return e;
    case EXPR_CALL:
        for (auto &a : e->args) a = optimize_expr(std::move(a));
        return e;
    case EXPR_ARRAY:
        for (auto &el : e->elems) el = optimize_expr(std::move(el));
        return e;
    case EXPR_STRUCT:
        for (auto &f : e->fields) f.value = optimize_expr(std::move(f.value));
        return e;
    default: // литералы, идентификаторы, EXPR_SCOPE
        return e;
    }
}

static void optimize_stmt_into(std::unique_ptr<Stmt> s, Block &out) {
    switch (s->kind) {
    case STMT_IF: {
        s->cond = optimize_expr(std::move(s->cond));
        s->body = optimize_block(s->body);
        if (s->has_else)
            s->else_body = optimize_block(s->else_body);

        if (s->cond->kind == EXPR_BOOL) {
            Block &taken = s->cond->bval ? s->body : s->else_body;
            bool has_taken = s->cond->bval || s->has_else;
            if (has_taken)
                for (auto &ts : taken) out.push_back(std::move(ts));
            return; // мёртвая ветка - весь if удалён
        }
        out.push_back(std::move(s));
        return;
    }
    case STMT_WHILE: {
        s->cond = optimize_expr(std::move(s->cond));
        if (s->cond->kind == EXPR_BOOL && !s->cond->bval)
            return; // while (false) — тело никогда не выполняется
        s->body = optimize_block(s->body);
        out.push_back(std::move(s));
        return;
    }
    case STMT_VAR:
        s->init = optimize_expr(std::move(s->init));
        out.push_back(std::move(s));
        return;
    case STMT_ASSIGN:
        s->target = optimize_expr(std::move(s->target));
        s->val = optimize_expr(std::move(s->val));
        out.push_back(std::move(s));
        return;
    case STMT_RETURN:
        if (s->ret_val) s->ret_val = optimize_expr(std::move(s->ret_val));
        out.push_back(std::move(s));
        return;
    case STMT_EXPR:
        s->expr = optimize_expr(std::move(s->expr));
        out.push_back(std::move(s));
        return;
    case STMT_BREAK:
    case STMT_CONTINUE:
        out.push_back(std::move(s));
        return;
    }
}

static Block optimize_block(Block &block) {
    Block out;
    for (auto &s : block)
        optimize_stmt_into(std::move(s), out);
    return out;
}

static void optimize_decl(Decl &d) {
    switch (d.kind) {
    case DECL_FUNC:
        d.body = optimize_block(d.body);
        break;
    case DECL_IMPL:
    case DECL_NAMESPACE:
        for (auto &sub : d.decls) optimize_decl(*sub);
        break;
    default:
        break;
    }
}

void optimize_program(Program *prog) {
    for (auto &d : prog->decls)
        optimize_decl(*d);
}

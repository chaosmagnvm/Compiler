#include "codegen.hpp"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static bool is_float_kind(TypeKind k) {
    return k == TYPE_FLOAT32 || k == TYPE_FLOAT64;
}

static bool is_sint_kind(TypeKind k) {
    return k == TYPE_INT8 || k == TYPE_INT16 || k == TYPE_INT32 || k == TYPE_INT64;
}

static bool is_uint_kind(TypeKind k) {
    return k == TYPE_UINT8 || k == TYPE_UINT16 || k == TYPE_UINT32 || k == TYPE_UINT64;
}

static bool is_int_kind(TypeKind k) {
    return is_sint_kind(k) || is_uint_kind(k) || k == TYPE_BOOL;
}

// одно поле структуры внутри StructLayout — смещение от начала структуры в байтах
struct StructField {
    std::string name;
    int offset;
    TypeKind kind;
    std::string sname; // имя типа, если kind == TYPE_NAMED (другая структура/алиас)
    TypeKind elem_kind; // тип элемента, если kind == TYPE_ARRAY
    int array_n; // длина массива, если kind == TYPE_ARRAY
};

// раскладка структуры в памяти: список полей со смещениями + общий размер в байтах
struct StructLayout {
    std::vector<StructField> fields;
    int total_size;
};

// тип одного параметра функции — нужен чтобы решить, идёт ли аргумент в xmm или в обычный регистр
struct FuncParam {
    TypeKind kind;
    bool is_float;
    std::string sname;
};

// сведения о функции, собранные на предварительном проходе collect_info
struct FuncInfo {
    std::vector<FuncParam> params;
    TypeKind ret_kind;
    std::string ret_sname;
    std::string mangled; // итоговое имя метки в ассемблере с учётом перегрузок
    const Decl *decl_ptr = nullptr;
};

// локальная переменная/параметр: смещение от rbp + информация о типе для кодогена
struct CgVar {
    int offset = 0;
    TypeKind kind = TYPE_INT64;
    std::string sname; // имя структуры/алиаса, если kind == TYPE_NAMED
    TypeKind elem_kind = TYPE_INT64; // тип элемента массива
    int array_n = 0; // длина массива
    bool is_ptr = false; // true для структур/массивов-параметров — в переменной лежит адрес, не значение
};

using Scope = std::map<std::string, CgVar>;

struct CgenCtx {
    std::ostream &out;
    int label_id = 0;
    std::map<std::string, StructLayout> structs;
    std::map<std::string, TypeKind> aliases; // type Alias = Base — для resolve_alias
    std::map<std::string, FuncInfo> funcs;   // по итоговому (mangled) имени функции
    // A.2.8: для каждого исходного имени — список mangled-имён всех его перегрузок ($orig, $0, $1, ...)
    std::map<std::string, std::vector<std::string>> overloads;
    std::map<const Decl*, std::string> decl_mangled; // DECL_FUNC -> его итоговое mangled-имя
    std::vector<Scope> scopes;
    std::string ret_label; // метка epilogue функции; return прыгает сюда, а не делает ret сам
    TypeKind ret_kind = TYPE_VOID;
    std::vector<std::string> break_stack; // стек меток конца цикла для break (вложенные циклы)
    std::vector<std::string> cont_stack; // стек меток условия цикла для continue
    std::vector<std::string> strlits; // строковые литералы — собираем во время обхода, выдаём в .rodata в конце
    std::vector<double> flitvals; // float-литералы — аналогично
    CodegenError err;
    bool had_error = false;
    std::string new_label() { return ".L" + std::to_string(label_id++); }
    std::string new_glabel() { return "_CL" + std::to_string(label_id++); }
};

static void cg_fail(CgenCtx &ctx, const std::string &msg, int line = 0, int col = 0) {
    if (ctx.had_error) return;
    ctx.err = {msg, line, col};
    ctx.had_error = true;
}

static void push_scope(CgenCtx &ctx) { ctx.scopes.push_back({}); }
static void pop_scope(CgenCtx &ctx) { ctx.scopes.pop_back(); }

static const CgVar *find_var(const CgenCtx &ctx, const std::string &name) {
    for (int i = (int)ctx.scopes.size() - 1; i >= 0; i--) {
        auto it = ctx.scopes[i].find(name);
        if (it != ctx.scopes[i].end()) return &it->second;
    }
    return nullptr;
}

static void define_var(CgenCtx &ctx, const std::string &name, CgVar v) {
    ctx.scopes.back()[name] = v;
}

// раскрываем именованный тип: структура остаётся TYPE_NAMED, алиас — до своего базового kind
static TypeKind resolve_alias(const CgenCtx &ctx, const std::string &name) {
    if (ctx.structs.count(name)) return TYPE_NAMED;
    auto it = ctx.aliases.find(name);
    if (it != ctx.aliases.end()) return it->second;
    return TYPE_INT64;
}

static TypeKind resolve_kind(const CgenCtx &ctx, const Type &t) {
    if (t.kind != TYPE_NAMED) return t.kind;
    return resolve_alias(ctx, t.name);
}

static bool is_float_type(const CgenCtx &ctx, const Type &t) {
    return is_float_kind(resolve_kind(ctx, t));
}

static bool is_struct_name(const CgenCtx &ctx, const std::string &name) {
    return ctx.structs.count(name) > 0;
}

// размер значения в байтах для слота на стеке скаляры всегда занимают 8 байт,
// у массивов и структур — реальный размер (элемент * длина / сумма полей)
static int slot_size(const CgenCtx &ctx, TypeKind k, const std::string &sname, TypeKind elem_k, int arr_n) {
    if (k == TYPE_ARRAY) {
        if (elem_k == TYPE_NAMED && ctx.structs.count(sname))
            return arr_n * ctx.structs.at(sname).total_size;
        return arr_n * 8;
    }
    if (k == TYPE_NAMED && ctx.structs.count(sname))
        return ctx.structs.at(sname).total_size;
    return 8;
}

// нужно при печати и при выборе оператора '+' (конкатенация vs сложение)
static bool expr_is_string(const CgenCtx &ctx, const Expr &e) {
    if (e.kind == EXPR_STRING) return true;
    if (e.kind == EXPR_IDENT) {
        const CgVar *v = find_var(ctx, e.sval);
        return v && v->kind == TYPE_STRING;
    }
    if (e.kind == EXPR_BINARY && e.binop == BINOP_ADD)
        return expr_is_string(ctx, *e.lhs) || (e.rhs && expr_is_string(ctx, *e.rhs));
    if (e.kind == EXPR_CALL && e.lhs && e.lhs->kind == EXPR_IDENT && e.lhs->sval == "input")
        return true;
    return false;
}

// определяем тип выражения без генерации кода — нужно заранее выбрать путь xmm или rax в EXPR_BINARY
static bool expr_is_float_r(const CgenCtx &ctx, const Expr &e) {
    switch (e.kind) {
    case EXPR_FLOAT:
        return true;

    case EXPR_IDENT: {
        const CgVar *v = find_var(ctx, e.sval);
        if (!v) return false;
        TypeKind k = (v->kind == TYPE_NAMED) ? resolve_alias(ctx, v->sname) : v->kind;
        return is_float_kind(k);
    }

    case EXPR_CAST:
        return is_float_kind(resolve_kind(ctx, e.cast_type));

    case EXPR_BINARY:
        if (e.binop == BINOP_AND || e.binop == BINOP_OR ||
            e.binop == BINOP_EQ || e.binop == BINOP_NEQ ||
            e.binop == BINOP_LT || e.binop == BINOP_GT ||
            e.binop == BINOP_LEQ || e.binop == BINOP_GEQ) return false;
        return (e.lhs && expr_is_float_r(ctx, *e.lhs)) ||
               (e.rhs && expr_is_float_r(ctx, *e.rhs));

    case EXPR_UNARY:
        return e.unop == UNOP_NEG && e.lhs && expr_is_float_r(ctx, *e.lhs);

    case EXPR_FIELD: {
        if (!e.lhs) return false;
        std::string sname;
        if (e.lhs->kind == EXPR_IDENT) {
            const CgVar *v = find_var(ctx, e.lhs->sval);
            if (v) sname = v->sname;
        }
        auto sit = ctx.structs.find(sname);
        if (sit != ctx.structs.end()) {
            for (const auto &f : sit->second.fields)
                if (f.name == e.sval) return is_float_kind(f.kind);
        }
        return false;
    }

    case EXPR_INDEX: {
        if (!e.lhs) return false;
        if (e.lhs->kind == EXPR_IDENT) {
            const CgVar *v = find_var(ctx, e.lhs->sval);
            if (v && v->kind == TYPE_ARRAY) return is_float_kind(v->elem_kind);
        }
        return false;
    }

    case EXPR_CALL: {
        if (!e.lhs) return false;
        std::string mn;
        if (e.lhs->kind == EXPR_IDENT) mn = "_cosmos_" + e.lhs->sval;
        // A.2.12: Struct::method уже разобран парсером как EXPR_SCOPE(Struct, method)
        else if (e.lhs->kind == EXPR_SCOPE && e.lhs->lhs)
            mn = "_cosmos_" + e.lhs->lhs->sval + "__" + e.lhs->sval;
        auto it = ctx.funcs.find(mn);
        if (it != ctx.funcs.end()) return is_float_kind(it->second.ret_kind);
        return false;
    }

    default:
        return false;
    }
}

static int slot_size_of_type(const CgenCtx &ctx, const Type &t) {
    if (t.kind == TYPE_ARRAY && t.elem)
        return t.array_size * slot_size_of_type(ctx, *t.elem);
    if (t.kind == TYPE_NAMED && ctx.structs.count(t.name))
        return ctx.structs.at(t.name).total_size;
    return 8;
}

// строит CgVar для переменной с явно указанным типом
static CgVar make_cgvar_from_type(int offset, const Type &t) {
    CgVar v;
    v.offset = offset;
    v.kind = t.kind;
    v.sname = t.name;
    v.elem_kind = TYPE_INT64;
    v.array_n = 0;
    if (t.kind == TYPE_ARRAY && t.elem) {
        v.elem_kind = t.elem->kind;
        v.array_n = t.array_size;
        if (t.elem->kind == TYPE_NAMED) v.sname = t.elem->name;
    }
    return v;
}

static void collect_info(CgenCtx &ctx, const DeclList &decls, const std::string &ns);

// раскладка структур, таблица псевдонимов,
// мена функций без учета перегрузок нужно знать заранее до генерации кода
static void collect_one(CgenCtx &ctx, const Decl &d, const std::string &ns) {
    std::string full = ns.empty() ? d.name : ns + "__" + d.name;

    if (d.kind == DECL_STRUCT) {
        // считаем смещения для структур
        // для массивов array_size * 8
        StructLayout layout;
        int off = 0;
        for (const auto &f : d.fields) {
            StructField sf;
            sf.name = f.name;
            sf.offset = off;
            sf.kind = f.type.kind;
            sf.sname = f.type.name;
            sf.elem_kind = TYPE_INT64;
            sf.array_n = 0;
            if (f.type.kind == TYPE_ARRAY && f.type.elem) {
                sf.elem_kind = f.type.elem->kind;
                sf.array_n = f.type.array_size;
            }
            int fsz = 8;
            if (f.type.kind == TYPE_ARRAY && f.type.elem)
                fsz = f.type.array_size * 8;
            layout.fields.push_back(sf);
            off += fsz;
        }
        layout.total_size = off ? off : 8;
        ctx.structs[d.name] = layout;
        return;
    }

    if (d.kind == DECL_TYPE_ALIAS) {
        // запоминаем только итоговый тип
        TypeKind k = d.alias_type.kind;
        if (k == TYPE_NAMED) {
            auto it = ctx.aliases.find(d.alias_type.name);
            if (it != ctx.aliases.end()) k = it->second;
            else if (ctx.structs.count(d.alias_type.name)) k = TYPE_NAMED;
        }
        ctx.aliases[d.name] = k;
        return;
    }

    if (d.kind == DECL_FUNC) {
        // main имя в ассемблере не меняем
        std::string base_mangled = (ns.empty() && d.name == "main") ? "main" : "_cosmos_" + full;

        FuncInfo info;
        info.ret_kind = d.ret_type.kind;
        info.ret_sname = d.ret_type.name;
        if (d.ret_type.kind == TYPE_NAMED) {
            auto it = ctx.aliases.find(d.ret_type.name);
            if (it != ctx.aliases.end()) info.ret_kind = it->second;
        }
        for (const auto &p : d.params) {
            FuncParam fp;
            fp.kind = p.type.kind;
            fp.sname = p.type.name;
            if (p.type.kind == TYPE_NAMED) {
                auto it = ctx.aliases.find(p.type.name);
                if (it != ctx.aliases.end()) fp.kind = it->second;
            }
            fp.is_float = is_float_kind(fp.kind);
            info.params.push_back(fp);
        }

        info.decl_ptr = &d;
        // A.2.8: имя уже занято — это перегрузка
        if (ctx.funcs.count(base_mangled)) {
            int idx = (int)ctx.overloads[base_mangled].size();
            if (idx == 0) {
                FuncInfo orig = ctx.funcs[base_mangled];
                orig.mangled = base_mangled + "$0";
                ctx.funcs[orig.mangled] = orig;
                ctx.overloads[base_mangled].push_back(orig.mangled);
                if (orig.decl_ptr)
                    ctx.decl_mangled[orig.decl_ptr] = base_mangled + "$0";
            }
            std::string versioned = base_mangled + "$" + std::to_string(idx + 1);
            info.mangled = versioned;
            ctx.funcs[versioned] = info;
            ctx.overloads[base_mangled].push_back(versioned);
            ctx.decl_mangled[&d] = versioned;
        } else {
            info.mangled = base_mangled;
            ctx.funcs[base_mangled] = info;
            ctx.decl_mangled[&d] = base_mangled;
        }
        return;
    }

    // namespace — просто префикс Name__
    if (d.kind == DECL_NAMESPACE)
        collect_info(ctx, d.decls, full);

    // A.2.12: impl StructName: — методы собираются как функции StructName::method,
    // имя метода будет выглядет ькак _cosmos_StructName__method
    if (d.kind == DECL_IMPL)
        collect_info(ctx, d.decls, d.struct_name);
}

static void collect_info(CgenCtx &ctx, const DeclList &decls, const std::string &ns) {
    for (const auto &d : decls) collect_one(ctx, *d, ns);
}

// назначаем rbp-смещения всем локальным переменным.
// делается до генерации кода, чтобы сразу выдать sub rsp, N в прологе
static int compute_frame(const CgenCtx &ctx, const Block &blk, int cur_off, std::map<const Stmt *, int> &offsets) {
    for (const auto &sp : blk) {
        const Stmt &s = *sp;
        if (s.kind == STMT_VAR) {
            int sz = 8;
            if (s.has_type) {
                sz = slot_size_of_type(ctx, s.var_type);
            } else if (s.init) {
                if (s.init->kind == EXPR_ARRAY && !s.init->elems.empty())
                    sz = (int)s.init->elems.size() * 8;
                else if (s.init->kind == EXPR_STRUCT) {
                    auto it = ctx.structs.find(s.init->sval);
                    if (it != ctx.structs.end()) sz = it->second.total_size;
                }
            }
            sz = (sz + 7) & ~7;
            cur_off -= sz;
            offsets[sp.get()] = cur_off;
        }
        if (s.kind == STMT_IF) {
            cur_off = compute_frame(ctx, s.body, cur_off, offsets);
            if (s.has_else)
                cur_off = compute_frame(ctx, s.else_body, cur_off, offsets);
        }
        if (s.kind == STMT_WHILE)
            cur_off = compute_frame(ctx, s.body, cur_off, offsets);
    }
    return cur_off;
}

// все локальные слоты лежат ниже rbp
static std::string rbp_ref(int offset) {
    if (offset >= 0) return "[rbp+" + std::to_string(offset) + "]";
    return "[rbp" + std::to_string(offset) + "]";
}

// загружает переменную в rax/xmm0; для массивов и структур кладёт в rax
// возвращает true если результат в xmm0 (как и cgen_expr)
static bool emit_load_var(CgenCtx &ctx, const CgVar &v) {
    auto &out = ctx.out;
    TypeKind ek = (v.kind == TYPE_NAMED) ? resolve_alias(ctx, v.sname) : v.kind;

    if (v.kind == TYPE_ARRAY || (v.kind == TYPE_NAMED && ctx.structs.count(v.sname))) {
        if (v.is_ptr) {
            out << "\tmov rax, " << rbp_ref(v.offset) << "\n";
        } else {
            out << "\tlea rax, " << rbp_ref(v.offset) << "\n";
        }
        return false;
    }
    if (is_float_kind(ek)) {
        out << "\tmovsd xmm0, " << rbp_ref(v.offset) << "\n";
        return true;
    }
    out << "\tmov rax, " << rbp_ref(v.offset) << "\n";
    return false;
}

static void emit_store_var(CgenCtx &ctx, const CgVar &v, bool is_float) {
    auto &out = ctx.out;
    TypeKind ek = (v.kind == TYPE_NAMED) ? resolve_alias(ctx, v.sname) : v.kind;
    if (is_float_kind(ek) || is_float) {
        out << "\tmovsd " << rbp_ref(v.offset) << ", xmm0\n";
    } else {
        out << "\tmov qword ptr " << rbp_ref(v.offset) << ", rax\n";
    }
}

// возвращает true если результат в xmm0, false если в rax
static bool cgen_expr(CgenCtx &ctx, const Expr &e, const std::map<const Stmt *, int> &offsets);

// вычисляет адрес выражения-lvalue в rax 
static bool cgen_lvalue_addr(CgenCtx &ctx, const Expr &e, const std::map<const Stmt *, int> &offsets) {
    auto &out = ctx.out;
    if (e.kind == EXPR_IDENT) {
        const CgVar *v = find_var(ctx, e.sval);
        if (!v) { cg_fail(ctx, "unknown var: " + e.sval, e.line, e.col); return false; }
        out << "\tlea rax, " << rbp_ref(v->offset) << "\n";
        return true;
    }
    if (e.kind == EXPR_INDEX) {
        // arr[i] -> адрес массива
        cgen_expr(ctx, *e.lhs, offsets);
        out << "\tpush rax\n";
        cgen_expr(ctx, *e.rhs, offsets);
        out << "\timul rax, 8\n";
        out << "\tpop rcx\n";
        out << "\tadd rax, rcx\n";
        return true;
    }
    if (e.kind == EXPR_FIELD) {
        // obj.field адрес obj + смещение поля из StructLayout
        cgen_expr(ctx, *e.lhs, offsets);
        std::string sname;
        if (e.lhs->kind == EXPR_IDENT) {
            const CgVar *v = find_var(ctx, e.lhs->sval);
            if (v) sname = v->sname;
        }
        if (sname.empty() && e.lhs->kind == EXPR_STRUCT) sname = e.lhs->sval;
        auto sit = ctx.structs.find(sname);
        if (sit != ctx.structs.end()) {
            for (const auto &f : sit->second.fields) {
                if (f.name == e.sval) {
                    if (f.offset) out << "\tadd rax, " << f.offset << "\n";
                    return true;
                }
            }
        }
        cg_fail(ctx, "field not found: " + e.sval, e.line, e.col);
        return false;
    }
    cg_fail(ctx, "invalid lvalue", e.line, e.col);
    return false;
}

// A.2.12: имя структуры, к которой относится выражение нужно чтобы найти метод
static std::string expr_struct_name(const CgenCtx &ctx, const Expr &e) {
    if (e.kind == EXPR_IDENT) {
        const CgVar *v = find_var(ctx, e.sval);
        if (v) return v->sname;
    }
    if (e.kind == EXPR_STRUCT) return e.sval;
    if (e.kind == EXPR_INDEX && e.lhs) {
        const CgVar *v = (e.lhs->kind == EXPR_IDENT) ? find_var(ctx, e.lhs->sval) : nullptr;
        if (v && v->kind == TYPE_ARRAY) return v->sname;
    }
    return "";
}

static bool cgen_expr(CgenCtx &ctx, const Expr &e, const std::map<const Stmt *, int> &offsets) {
    if (ctx.had_error) return false;
    auto &out = ctx.out;
    int lid = ctx.label_id;

    switch (e.kind) {

    case EXPR_INT:
        out << "\tmov rax, " << e.ival << "\n";
        return false;

    case EXPR_FLOAT: {
        // x86 не может закодировать double прямо в инструкции — кладём в .rodata и грузим через rip
        int idx = (int)ctx.flitvals.size();
        ctx.flitvals.push_back(e.fval);
        out << "\tmovsd xmm0, _flt_" << idx << "[rip]\n";
        return true;
    }

    case EXPR_BOOL:
        out << "\tmov rax, " << (e.bval ? 1 : 0) << "\n";
        return false;

    case EXPR_STRING: {
        // как и с float-литералами — сама строка уходит в .rodata, здесь только адрес
        int idx = (int)ctx.strlits.size();
        ctx.strlits.push_back(e.sval);
        out << "\tlea rax, _str_" << idx << "[rip]\n";
        return false;
    }

    case EXPR_IDENT: {
        const CgVar *v = find_var(ctx, e.sval);
        if (!v) {
            cg_fail(ctx, "unknown identifier: " + e.sval, e.line, e.col);
            return false;
        }
        return emit_load_var(ctx, *v);
    }

    case EXPR_BINARY: {
        ctx.label_id++;
        int id = lid;

        // && и || — короткое замыкание: rhs не вычисляется, если результат уже ясен по lhs
        if (e.binop == BINOP_AND) {
            std::string lfalse = ".Land_f" + std::to_string(id);
            std::string lend = ".Land_e" + std::to_string(id);
            cgen_expr(ctx, *e.lhs, offsets);
            out << "\ttest rax, rax\n\tjz " << lfalse << "\n";
            cgen_expr(ctx, *e.rhs, offsets);
            out << "\ttest rax, rax\n\tjz " << lfalse << "\n";
            out << "\tmov rax, 1\n\tjmp " << lend << "\n";
            out << lfalse << ":\n\txor rax, rax\n";
            out << lend << ":\n";
            return false;
        }
        if (e.binop == BINOP_OR) {
            std::string ltrue = ".Lor_t" + std::to_string(id);
            std::string lend = ".Lor_e" + std::to_string(id);
            cgen_expr(ctx, *e.lhs, offsets);
            out << "\ttest rax, rax\n\tjnz " << ltrue << "\n";
            cgen_expr(ctx, *e.rhs, offsets);
            out << "\ttest rax, rax\n\tjnz " << ltrue << "\n";
            out << "\txor rax, rax\n\tjmp " << lend << "\n";
            out << ltrue << ":\n\tmov rax, 1\n";
            out << lend << ":\n";
            return false;
        }

        // строки сравниваются по содержимому (strcmp), а не по адресу указателя
        if (e.lhs && expr_is_string(ctx, *e.lhs) &&
            (e.binop == BINOP_EQ || e.binop == BINOP_NEQ)) {
            cgen_expr(ctx, *e.lhs, offsets);
            out << "\tpush rax\n";
            cgen_expr(ctx, *e.rhs, offsets);
            out << "\tmov rsi, rax\n\tpop rdi\n";
            out << "\tcall strcmp\n";
            out << "\ttest eax, eax\n";
            if (e.binop == BINOP_EQ) out << "\tsete al\n";
            else out << "\tsetne al\n";
            out << "\tmovzx rax, al\n";
            return false;
        }

        // '+' на строках — конкатенация через runtime-хелпер (выделяет новую строку)
        if (e.binop == BINOP_ADD && e.lhs && expr_is_string(ctx, *e.lhs)) {
            cgen_expr(ctx, *e.lhs, offsets);
            out << "\tpush rax\n";
            cgen_expr(ctx, *e.rhs, offsets);
            out << "\tmov rsi, rax\n\tpop rdi\n";
            out << "\tcall _cosmos_str_concat\n";
            return false;
        }

        // если хоть один из операндов float — оба считаются как float (неявное расширение)
        bool use_float = (e.lhs && expr_is_float_r(ctx, *e.lhs)) ||
                         (e.rhs && expr_is_float_r(ctx, *e.rhs));

        if (!use_float) {
            bool lf = cgen_expr(ctx, *e.lhs, offsets);
            (void)lf;
            out << "\tpush rax\n";
            cgen_expr(ctx, *e.rhs, offsets);
            out << "\tmov rcx, rax\n\tpop rax\n";

            switch (e.binop) {
            case BINOP_ADD:
                out << "\tadd rax, rcx\n";
                break;
            case BINOP_SUB:
                out << "\tsub rax, rcx\n";
                break;
            case BINOP_MUL:
                out << "\timul rax, rcx\n";
                break;
            case BINOP_DIV:
                // деление на ноль- проверяем сами и зовём panic с понятным сообщением
                out << "\ttest rcx, rcx\n";
                out << "\tjnz .Ddiv_ok" << id << "\n";
                out << "\tlea rdi, _err_divzero[rip]\n";
                out << "\tcall _cosmos_panic\n";
                out << ".Ddiv_ok" << id << ":\n";
                out << "\tcqo\n\tidiv rcx\n";
                break;
            case BINOP_MOD:
                out << "\ttest rcx, rcx\n";
                out << "\tjnz .Dmod_ok" << id << "\n";
                out << "\tlea rdi, _err_divzero[rip]\n";
                out << "\tcall _cosmos_panic\n";
                out << ".Dmod_ok" << id << ":\n";
                out << "\tcqo\n\tidiv rcx\n\tmov rax, rdx\n"; // остаток от деления — в rdx
                break;
            case BINOP_EQ:
                out << "\tcmp rax, rcx\n\tsete al\n\tmovzx rax, al\n";
                break;
            case BINOP_NEQ:
                out << "\tcmp rax, rcx\n\tsetne al\n\tmovzx rax, al\n";
                break;
            case BINOP_LT:
                out << "\tcmp rax, rcx\n\tsetl al\n\tmovzx rax, al\n";
                break;
            case BINOP_GT:
                out << "\tcmp rax, rcx\n\tsetg al\n\tmovzx rax, al\n";
                break;
            case BINOP_LEQ:
                out << "\tcmp rax, rcx\n\tsetle al\n\tmovzx rax, al\n";
                break;
            case BINOP_GEQ:
                out << "\tcmp rax, rcx\n\tsetge al\n\tmovzx rax, al\n";
                break;
            default:
                break;
            }
            return false;
        } else {
            // lhs сохраняем на стек — иначе rhs перезапишет xmm0
            bool lf = cgen_expr(ctx, *e.lhs, offsets);
            if (!lf) out << "\tcvtsi2sd xmm0, rax\n";
            out << "\tsub rsp, 8\n\tmovsd [rsp], xmm0\n";
            bool rf = cgen_expr(ctx, *e.rhs, offsets);
            if (!rf) out << "\tcvtsi2sd xmm0, rax\n";
            out << "\tmovsd xmm1, [rsp]\n\tadd rsp, 8\n";

            switch (e.binop) {
            case BINOP_ADD:
                out << "\taddsd xmm1, xmm0\n\tmovsd xmm0, xmm1\n";
                return true;
            case BINOP_SUB:
                out << "\tsubsd xmm1, xmm0\n\tmovsd xmm0, xmm1\n";
                return true;
            case BINOP_MUL:
                out << "\tmulsd xmm1, xmm0\n\tmovsd xmm0, xmm1\n";
                return true;
            case BINOP_DIV:
                out << "\tdivsd xmm1, xmm0\n\tmovsd xmm0, xmm1\n";
                return true;
            case BINOP_EQ:
                out << "\tucomisd xmm1, xmm0\n\tsete al\n\tmovzx rax, al\n";
                return false;
            case BINOP_NEQ:
                out << "\tucomisd xmm1, xmm0\n\tsetne al\n\tmovzx rax, al\n";
                return false;
            case BINOP_LT:
                out << "\tucomisd xmm1, xmm0\n\tsetb al\n\tmovzx rax, al\n";
                return false;
            case BINOP_GT:
                out << "\tucomisd xmm0, xmm1\n\tsetb al\n\tmovzx rax, al\n";
                return false;
            case BINOP_LEQ:
                out << "\tucomisd xmm1, xmm0\n\tsetbe al\n\tmovzx rax, al\n";
                return false;
            case BINOP_GEQ:
                out << "\tucomisd xmm0, xmm1\n\tsetbe al\n\tmovzx rax, al\n";
                return false;
            default:
                return false;
            }
        }
    }

    case EXPR_UNARY: {
        bool is_f = cgen_expr(ctx, *e.lhs, offsets);
        if (e.unop == UNOP_NOT) {
            out << "\ttest rax, rax\n\tsete al\n\tmovzx rax, al\n";
            return false;
        }
        if (is_f) {
            out << "\tpxor xmm1, xmm1\n\tsubsd xmm1, xmm0\n\tmovsd xmm0, xmm1\n";
            return true;
        }
        out << "\tneg rax\n";
        return false;
    }

    case EXPR_CAST: {
        bool src_float = cgen_expr(ctx, *e.lhs, offsets);
        TypeKind dst = resolve_kind(ctx, e.cast_type);

        if (src_float && is_float_kind(dst)) return true;
        if (!src_float && is_int_kind(dst)) return false;

        if (src_float && is_int_kind(dst)) {
            out << "\tcvttsd2si rax, xmm0\n";
            return false;
        }
        if (!src_float && is_float_kind(dst)) {
            out << "\tcvtsi2sd xmm0, rax\n";
            return true;
        }
        return src_float;
    }

    case EXPR_INDEX: {
        std::string arr_name;
        TypeKind elem_k = TYPE_INT64;
        bool elem_float = false;
        if (e.lhs->kind == EXPR_IDENT) {
            const CgVar *v = find_var(ctx, e.lhs->sval);
            if (v && v->kind == TYPE_ARRAY) {
                arr_name = v->sname;
                elem_k = v->elem_kind;
                elem_float = is_float_kind(elem_k);
            }
        }

        cgen_expr(ctx, *e.lhs, offsets);
        out << "\tpush rax\n";
        cgen_expr(ctx, *e.rhs, offsets);

        out << "\timul rax, 8\n";
        out << "\tpop rcx\n";
        out << "\tadd rcx, rax\n";

        if (elem_float) {
            out << "\tmovsd xmm0, [rcx]\n";
            return true;
        }
        // элемент-структура — у него нет единого значения, оставляем в rax его адрес
        if (elem_k == TYPE_NAMED && ctx.structs.count(arr_name)) {
            out << "\tmov rax, rcx\n";
            return false;
        }
        out << "\tmov rax, [rcx]\n";
        return false;
    }

    case EXPR_FIELD: {
        // lhs даёт адрес объекта в rax, дальше просто + смещение
        std::string sname = expr_struct_name(ctx, *e.lhs);
        cgen_expr(ctx, *e.lhs, offsets);

        auto sit = ctx.structs.find(sname);
        if (sit == ctx.structs.end()) {
            cg_fail(ctx, "struct not found for field access: " + sname, e.line, e.col);
            return false;
        }
        for (const auto &f : sit->second.fields) {
            if (f.name == e.sval) {
                if (f.offset) out << "\tadd rax, " << f.offset << "\n";
                // поле-структура: в rax остаётся адрес, не значение
                if (f.kind == TYPE_NAMED && ctx.structs.count(f.sname)) {
                    return false;
                }
                if (is_float_kind(f.kind)) {
                    out << "\tmovsd xmm0, [rax]\n";
                    return true;
                }
                out << "\tmov rax, [rax]\n";
                return false;
            }
        }
        cg_fail(ctx, "field not found: " + e.sval, e.line, e.col);
        return false;
    }

    case EXPR_SCOPE:
        cg_fail(ctx, "scope expression outside of call", e.line, e.col);
        return false;

    case EXPR_CALL: {
        std::string mangled;
        bool is_builtin = false;
        bool is_method = false;
        const Expr *self_expr = nullptr;
        static const char *BUILTINS[] = {"print","input","exit","panic","len", nullptr};

        if (e.lhs->kind == EXPR_IDENT) {
            for (int i = 0; BUILTINS[i]; i++)
                if (e.lhs->sval == BUILTINS[i]) { is_builtin = true; break; }
            if (!is_builtin) {
                mangled = "_cosmos_" + e.lhs->sval;
                if (e.lhs->sval == "main") mangled = "main";
            }
        } else if (e.lhs->kind == EXPR_SCOPE &&
                   e.lhs->lhs && e.lhs->lhs->kind == EXPR_IDENT) {
            // A.2.12: Struct::method(self, ...) — явный вызов метода, self передан как обычный аргумент
            mangled = "_cosmos_" + e.lhs->lhs->sval + "__" + e.lhs->sval;
        } else if (e.lhs->kind == EXPR_FIELD && e.lhs->lhs) {
            // A.2.12: obj.method(args) — превращаем в вызов Struct::method(obj, args),
            // obj становится неявным self (self_expr), его сгенерируем первым аргументом ниже
            std::string struct_name = expr_struct_name(ctx, *e.lhs->lhs);
            if (!struct_name.empty()) {
                std::string mn = "_cosmos_" + struct_name + "__" + e.lhs->sval;
                if (ctx.funcs.count(mn) || ctx.overloads.count(mn)) {
                    mangled = mn;
                    is_method = true;
                    self_expr = e.lhs->lhs.get();
                }
            }
            if (!is_method) {
                cg_fail(ctx, "method not found: " + e.lhs->sval, e.line, e.col);
                return false;
            }
        } else {
            cg_fail(ctx, "complex call not supported", e.line, e.col);
            return false;
        }

        // A.2.8 / A.3.1: имя — это набор перегрузок, ищем версию с совпадающими по
        // float/int параметрами (semantic.cpp уже проверил совместимость с учётом can_widen,
        // здесь достаточно различать только xmm vs обычный регистр — base пропускает self у методов)
        if (!is_builtin && ctx.overloads.count(mangled)) {
            const auto &versions = ctx.overloads.at(mangled);
            for (const auto &vname : versions) {
                auto fit = ctx.funcs.find(vname);
                if (fit == ctx.funcs.end()) continue;
                size_t base = is_method ? 1 : 0;
                if (fit->second.params.size() != e.args.size() + base) continue;
                bool ok = true;
                for (size_t i = 0; i < e.args.size(); i++) {
                    bool arg_float = expr_is_float_r(ctx, *e.args[i]);
                    bool param_float = fit->second.params[i + base].is_float;
                    if (arg_float != param_float) { ok = false; break; }
                }
                if (ok) { mangled = fit->second.mangled; break; }
            }
        }

        // встроенные функции — не настоящие вызовы, разворачиваются прямо в код/runtime-хелперы
        if (is_builtin) {
            const std::string &bname = e.lhs->sval;

            if (bname == "exit") {
                if (!e.args.empty()) cgen_expr(ctx, *e.args[0], offsets);
                else out << "\txor edi, edi\n";
                out << "\tmov edi, eax\n\tcall exit\n";
                return false;
            }

            if (bname == "panic") {
                if (!e.args.empty()) {
                    cgen_expr(ctx, *e.args[0], offsets);
                    out << "\tmov rdi, rax\n";
                } else {
                    out << "\tlea rdi, _str_empty[rip]\n";
                }
                out << "\tcall _cosmos_panic\n";
                return false;
            }

            if (bname == "input") {
                out << "\tcall _cosmos_input\n";
                return false;
            }

            // len(строка) - strlen в рантайме; len(массив) -известная на этапе компиляции длина
            if (bname == "len") {
                if (e.args.empty()) { cg_fail(ctx, "len() requires argument", e.line, e.col); return false; }
                const Expr &arg = *e.args[0];
                bool arg_str = (arg.kind == EXPR_STRING) ||
                               (arg.kind == EXPR_IDENT && [&]{
                                   const CgVar *v = find_var(ctx, arg.sval);
                                   return v && v->kind == TYPE_STRING;
                               }());
                if (arg_str) {
                    cgen_expr(ctx, arg, offsets);
                    out << "\tmov rdi, rax\n\tcall strlen\n";
                    return false;
                }
                if (arg.kind == EXPR_IDENT) {
                    const CgVar *v = find_var(ctx, arg.sval);
                    if (v && v->kind == TYPE_ARRAY) {
                        out << "\tmov rax, " << v->array_n << "\n";
                        return false;
                    }
                }
                if (arg.kind == EXPR_ARRAY) {
                    out << "\tmov rax, " << arg.elems.size() << "\n";
                    return false;
                }
                cg_fail(ctx, "len() requires string or array", e.line, e.col);
                return false;
            }

            // print (целое/float/bool/строка/массив) определяем статически здесь же,
            if (bname == "print") {
                if (e.args.empty()) { out << "\tcall _cosmos_print_nl\n"; return false; }
                const Expr &arg = *e.args[0];

                TypeKind ak = TYPE_INT64;
                bool arg_float = false;
                bool arg_str = false;
                bool arg_bool = false;
                bool arg_arr = false;

                arg_float = expr_is_float_r(ctx, arg);
                arg_str = expr_is_string(ctx, arg);

                if (arg.kind == EXPR_BINARY &&
                    (arg.binop == BINOP_EQ || arg.binop == BINOP_NEQ ||
                     arg.binop == BINOP_LT || arg.binop == BINOP_GT ||
                     arg.binop == BINOP_LEQ || arg.binop == BINOP_GEQ ||
                     arg.binop == BINOP_AND || arg.binop == BINOP_OR)) arg_bool = true;
                if (arg.kind == EXPR_UNARY && arg.unop == UNOP_NOT) arg_bool = true;
                if (arg.kind == EXPR_BOOL) arg_bool = true;

                if (arg.kind == EXPR_INT) ak = TYPE_INT64;
                else if (arg_float) { ak = TYPE_FLOAT64; }
                else if (arg.kind == EXPR_BOOL) { ak = TYPE_BOOL; arg_bool = true; }
                else if (arg_str) { ak = TYPE_STRING; }
                else if (arg.kind == EXPR_IDENT) {
                    const CgVar *v = find_var(ctx, arg.sval);
                    if (v) {
                        TypeKind ek = (v->kind == TYPE_NAMED) ? resolve_alias(ctx, v->sname) : v->kind;
                        ak = ek;
                        arg_float = arg_float || is_float_kind(ek);
                        arg_str = arg_str || (ek == TYPE_STRING);
                        arg_bool = arg_bool || (ek == TYPE_BOOL);
                        arg_arr = (v->kind == TYPE_ARRAY);
                    }
                } else if (arg.kind == EXPR_CAST) {
                    TypeKind ck = resolve_kind(ctx, arg.cast_type);
                    arg_float = is_float_kind(ck);
                } else if (arg.kind == EXPR_CALL) {
                    std::string mn;
                    if (arg.lhs && arg.lhs->kind == EXPR_IDENT)
                        mn = "_cosmos_" + arg.lhs->sval;
                    else if (arg.lhs && arg.lhs->kind == EXPR_SCOPE &&
                             arg.lhs->lhs && arg.lhs->lhs->kind == EXPR_IDENT)
                        mn = "_cosmos_" + arg.lhs->lhs->sval + "__" + arg.lhs->sval;
                    auto fit = ctx.funcs.find(mn);
                    if (fit != ctx.funcs.end()) {
                        ak = fit->second.ret_kind;
                        arg_float = is_float_kind(ak);
                        arg_str = (ak == TYPE_STRING);
                        arg_bool = (ak == TYPE_BOOL);
                    }
                }

                // печать массива — вручную идём по элементам и оборачиваем в [a, b, c]
                if (arg_arr) {
                    const CgVar *v = (arg.kind == EXPR_IDENT) ? find_var(ctx, arg.sval) : nullptr;
                    if (v) {
                        int n = v->array_n;
                        out << "\tlea rdi, _fmt_arr_open[rip]\n\txor al, al\n\tcall printf\n";
                        for (int i = 0; i < n; i++) {
                            out << "\tlea rax, " << rbp_ref(v->offset) << "\n";
                            if (is_float_kind(v->elem_kind)) {
                                out << "\tmovsd xmm0, [rax+" << (i*8) << "]\n";
                                out << "\tlea rdi, _fmt_float[rip]\n\tmov al, 1\n\tcall printf\n";
                            } else if (v->elem_kind == TYPE_STRING) {
                                out << "\tmov rsi, [rax+" << (i*8) << "]\n";
                                out << "\tlea rdi, _fmt_str_plain[rip]\n\txor al, al\n\tcall printf\n";
                            } else if (v->elem_kind == TYPE_BOOL) {
                                out << "\tmov rax, [rax+" << (i*8) << "]\n";
                                std::string lt = ".Lpbt" + std::to_string(ctx.label_id);
                                std::string le = ".Lpbe" + std::to_string(ctx.label_id++);
                                out << "\ttest rax, rax\n\tjnz " << lt << "\n";
                                out << "\tlea rdi, _str_false[rip]\n\tjmp " << le << "\n";
                                out << lt << ":\n\tlea rdi, _str_true[rip]\n";
                                out << le << ":\n";
                                out << "\tlea rdi, _fmt_str_plain[rip]\n";
                                out << "\tlea rax, " << rbp_ref(v->offset) << "\n";
                                out << "\tmov rax, [rax+" << (i*8) << "]\n";
                                out << "\ttest rax, rax\n";
                                std::string lt2 = ".Lpbt2_" + std::to_string(ctx.label_id);
                                std::string le2 = ".Lpbe2_" + std::to_string(ctx.label_id++);
                                out << "\tjnz " << lt2 << "\n";
                                out << "\tlea rsi, _str_false[rip]\n\tjmp " << le2 << "\n";
                                out << lt2 << ":\n\tlea rsi, _str_true[rip]\n";
                                out << le2 << ":\n";
                                out << "\tlea rdi, _fmt_str_plain[rip]\n\txor al, al\n\tcall printf\n";
                            } else {
                                out << "\tmov rsi, [rax+" << (i*8) << "]\n";
                                out << "\tlea rdi, _fmt_int[rip]\n\txor al, al\n\tcall printf\n";
                            }
                            if (i < n-1) {
                                out << "\tlea rdi, _fmt_arr_sep[rip]\n\txor al, al\n\tcall printf\n";
                            }
                        }
                        out << "\tlea rdi, _fmt_arr_close[rip]\n\txor al, al\n\tcall printf\n";
                        return false;
                    }
                }

                bool is_f = cgen_expr(ctx, arg, offsets);

                if (arg_float || is_f) {
                    out << "\tlea rdi, _fmt_float[rip]\n\tmov al, 1\n\tcall printf\n";
                } else if (arg_str) {
                    out << "\tmov rsi, rax\n\tlea rdi, _fmt_str[rip]\n\txor al, al\n\tcall printf\n";
                } else if (arg_bool) {
                    std::string lt = ".Lpbt_" + std::to_string(ctx.label_id);
                    std::string le = ".Lpbe_" + std::to_string(ctx.label_id++);
                    out << "\ttest rax, rax\n\tjnz " << lt << "\n";
                    out << "\tlea rsi, _str_false[rip]\n\tjmp " << le << "\n";
                    out << lt << ":\n\tlea rsi, _str_true[rip]\n";
                    out << le << ":\n";
                    out << "\tlea rdi, _fmt_str[rip]\n\txor al, al\n\tcall printf\n";
                } else {
                    if (is_uint_kind(ak)) {
                        out << "\tmov rsi, rax\n\tlea rdi, _fmt_uint[rip]\n\txor al, al\n\tcall printf\n";
                    } else {
                        out << "\tmov rsi, rax\n\tlea rdi, _fmt_int[rip]\n\txor al, al\n\tcall printf\n";
                    }
                }
                return false;
            }
            return false;
        }

        auto fit = ctx.funcs.find(mangled);
        if (fit == ctx.funcs.end()) {
            cg_fail(ctx, "unknown function: " + mangled, e.line, e.col);
            return false;
        }
        const FuncInfo &fi = fit->second;
        // A.2.12: self (если есть) — невидимый нулевой аргумент перед явными e.args
        int self_count = is_method ? 1 : 0;
        int nargs = (int)e.args.size() + self_count;

        // сначала складываем все аргументы во временную область на стеке —
        // вычисление каждого следующего аргумента может затереть регистры,
        // в которые мы бы положили предыдущие, поэтому раскладка по регистрам идёт отдельным проходом
        int temp_size = ((nargs * 8) + 15) & ~15;
        if (temp_size > 0) out << "\tsub rsp, " << temp_size << "\n";

        if (is_method && self_expr) {
            cgen_expr(ctx, *self_expr, offsets);
            out << "\tmov qword ptr [rsp+0], rax\n";
        }

        for (int i = 0; i < (int)e.args.size(); i++) {
            int slot = i + self_count;
            bool af = cgen_expr(ctx, *e.args[i], offsets);
            if (af)
                out << "\tmovsd [rsp+" << (slot * 8) << "], xmm0\n";
            else
                out << "\tmov qword ptr [rsp+" << (slot * 8) << "], rax\n";
        }

        // System V AMD64: целочисленные/указатели — в rdi,rsi,rdx,rcx,r8,r9 по порядку,
        // float/double — отдельно в xmm0,xmm1 - (свой счётчик fi2)
        static const char *INT_REGS[] = {"rdi","rsi","rdx","rcx","r8","r9"};
        int ii = 0, fi2 = 0;
        for (int i = 0; i < nargs; i++) {
            bool param_float = (i < (int)fi.params.size()) ? fi.params[i].is_float : false;
            if (param_float) {
                out << "\tmovsd xmm" << fi2 << ", [rsp+" << (i*8) << "]\n";
                fi2++;
            } else {
                if (ii < 6) {
                    out << "\tmov " << INT_REGS[ii] << ", [rsp+" << (i*8) << "]\n";
                    ii++;
                }
            }
        }

        // System V AMD64 al = количество xmm-аргументов 
        if (fi2 > 0) out << "\tmov al, " << fi2 << "\n";
        else out << "\txor al, al\n";

        out << "\tcall " << mangled << "\n";
        if (temp_size > 0) out << "\tadd rsp, " << temp_size << "\n";

        return is_float_kind(fi.ret_kind);
    }

    case EXPR_ARRAY:
        out << "\t; EXPR_ARRAY standalone (should not appear here)\n";
        return false;

    case EXPR_STRUCT:
        out << "\t; EXPR_STRUCT standalone (should not appear here)\n";
        return false;

    default:
        cg_fail(ctx, "unhandled expression kind", e.line, e.col);
        return false;
    }
}

static bool cgen_block(CgenCtx &ctx, const Block &blk, const std::map<const Stmt *, int> &offsets);

static bool cgen_stmt(CgenCtx &ctx, const Stmt &s, const std::map<const Stmt *, int> &offsets) {
    if (ctx.had_error) return false;
    auto &out = ctx.out;

    switch (s.kind) {

    case STMT_VAR: {
        auto oit = offsets.find(&s);
        if (oit == offsets.end()) {
            cg_fail(ctx, "var offset not found: " + s.var_name, s.line, s.col);
            return false;
        }
        int off = oit->second;

        CgVar v;
        v.offset = off;
        v.elem_kind = TYPE_INT64;
        v.array_n = 0;

        if (s.has_type) {
            v = make_cgvar_from_type(off, s.var_type);
            if (v.kind == TYPE_NAMED && !ctx.structs.count(v.sname)) {
                TypeKind rk = resolve_alias(ctx, v.sname);
                v.kind = rk;
            }
        } else if (s.init) {
            // A.1.7: тип не указан (let x = ...) — определяем CgVar по виду инициализатора,
            const Expr &ie = *s.init;
            if (ie.kind == EXPR_INT) { v.kind = TYPE_INT64; }
            else if (ie.kind == EXPR_FLOAT) { v.kind = TYPE_FLOAT64; }
            else if (ie.kind == EXPR_BOOL) { v.kind = TYPE_BOOL; }
            else if (ie.kind == EXPR_STRING) { v.kind = TYPE_STRING; }
            else if (ie.kind == EXPR_STRUCT) { v.kind = TYPE_NAMED; v.sname = ie.sval; }
            else if (ie.kind == EXPR_ARRAY) {
                v.kind = TYPE_ARRAY;
                v.array_n = (int)ie.elems.size();
                if (!ie.elems.empty()) {
                    const Expr &e0 = *ie.elems[0];
                    if (e0.kind == EXPR_FLOAT) v.elem_kind = TYPE_FLOAT64;
                    else if (e0.kind == EXPR_STRING) v.elem_kind = TYPE_STRING;
                    else if (e0.kind == EXPR_BOOL) v.elem_kind = TYPE_BOOL;
                    else if (e0.kind == EXPR_STRUCT) { v.elem_kind = TYPE_NAMED; v.sname = e0.sval; }
                    else v.elem_kind = TYPE_INT64;
                }
            } else if (ie.kind == EXPR_IDENT) {
                const CgVar *src = find_var(ctx, ie.sval);
                if (src) {
                    v.kind = src->kind;
                    v.sname = src->sname;
                    v.elem_kind = src->elem_kind;
                    v.array_n = src->array_n;
                } else v.kind = TYPE_INT64;
            } else if (ie.kind == EXPR_CALL) {
                // let x = f(...) / obj.method(...) — тип x берём из возвращаемого типа функции;
                // последняя ветка (EXPR_FIELD) — A.2.12, тот же способ найти Struct::method, что в cgen_expr
                std::string mn;
                if (ie.lhs && ie.lhs->kind == EXPR_IDENT) {
                    if (ie.lhs->sval == "input") { v.kind = TYPE_STRING; break; }
                    mn = "_cosmos_" + ie.lhs->sval;
                } else if (ie.lhs && ie.lhs->kind == EXPR_SCOPE &&
                           ie.lhs->lhs && ie.lhs->lhs->kind == EXPR_IDENT) {
                    mn = "_cosmos_" + ie.lhs->lhs->sval + "__" + ie.lhs->sval;
                } else if (ie.lhs && ie.lhs->kind == EXPR_FIELD && ie.lhs->lhs) {
                    std::string sn = expr_struct_name(ctx, *ie.lhs->lhs);
                    if (!sn.empty()) mn = "_cosmos_" + sn + "__" + ie.lhs->sval;
                }
                auto fit = ctx.funcs.find(mn);
                if (fit != ctx.funcs.end()) {
                    v.kind = fit->second.ret_kind;
                    v.sname = fit->second.ret_sname;
                } else v.kind = TYPE_INT64;
            } else if (expr_is_string(ctx, ie)) {
                v.kind = TYPE_STRING;
            } else if (expr_is_float_r(ctx, ie)) {
                v.kind = TYPE_FLOAT64;
            } else {
                v.kind = TYPE_INT64;
            }
        } else {
            v.kind = TYPE_INT64;
        }

        define_var(ctx, s.var_name, v);

        if (!s.init) return true;

        const Expr &init = *s.init;

        // [1, 2, 3]: каждый элемент в свой слот по 8 байт подряд от off
        if (init.kind == EXPR_ARRAY) {
            for (int i = 0; i < (int)init.elems.size(); i++) {
                bool is_f = cgen_expr(ctx, *init.elems[i], offsets);
                if (is_f)
                    out << "\tmovsd [rbp" << (off + i*8 >= 0 ? "+" : "") << (off + i*8) << "], xmm0\n";
                else
                    out << "\tmov qword ptr [rbp" << (off + i*8 >= 0 ? "+" : "") << (off + i*8) << "], rax\n";
            }
            return !ctx.had_error;
        }

        // StructName{field: val, ...} поля кладём по смещениям из StructLayout,
        // порядок в литерале может не совпадать с порядком объявления
        if (init.kind == EXPR_STRUCT) {
            auto sit = ctx.structs.find(init.sval);
            if (sit == ctx.structs.end()) {
                cg_fail(ctx, "unknown struct: " + init.sval, init.line, init.col);
                return false;
            }
            const StructLayout &layout = sit->second;
            for (const auto &sf : layout.fields) {
                const Expr *fval = nullptr;
                for (const auto &fi : init.fields)
                    if (fi.name == sf.name) { fval = fi.value.get(); break; }
                if (!fval) { cg_fail(ctx, "missing field: " + sf.name, init.line, init.col); return false; }
                bool is_f = cgen_expr(ctx, *fval, offsets);
                int foff = off + sf.offset;
                if (is_f)
                    out << "\tmovsd [rbp" << (foff >= 0 ? "+" : "") << foff << "], xmm0\n";
                else
                    out << "\tmov qword ptr [rbp" << (foff >= 0 ? "+" : "") << foff << "], rax\n";
            }
            return !ctx.had_error;
        }

        // инициализатор — другая переменная/выражение того же агрегатного типа
        // cgen_expr вернёт адрес (emit_load_var для структур/массивов), копируем целиком
        if ((v.kind == TYPE_NAMED && ctx.structs.count(v.sname)) || v.kind == TYPE_ARRAY) {
            int sz = slot_size(ctx, v.kind, v.sname, v.elem_kind, v.array_n);
            cgen_expr(ctx, init, offsets);
            out << "\tmov rsi, rax\n";
            out << "\tlea rdi, " << rbp_ref(off) << "\n";
            out << "\tmov rcx, " << sz << "\n";
            out << "\tcall memcpy\n";
            return !ctx.had_error;
        }

        bool is_f = cgen_expr(ctx, init, offsets);
        emit_store_var(ctx, v, is_f);
        return !ctx.had_error;
    }

    case STMT_ASSIGN: {
        const Expr &tgt = *s.target;
        const Expr &val = *s.val;

        // сперва выясняем тип цели (float/агрегат/обычное число) -- от этого зависит, нужен ли memcpy и куда положить результат val (xmm0 или rax)
        bool tgt_float = false;
        bool tgt_agg = false;
        std::string tgt_sname;
        TypeKind tgt_ek = TYPE_INT64;
        if (tgt.kind == EXPR_IDENT) {
            const CgVar *v = find_var(ctx, tgt.sval);
            if (v) {
                TypeKind ek = (v->kind == TYPE_NAMED) ? resolve_alias(ctx, v->sname) : v->kind;
                tgt_float = is_float_kind(ek);
                tgt_agg = (v->kind == TYPE_ARRAY) ||
                           (v->kind == TYPE_NAMED && ctx.structs.count(v->sname));
                tgt_sname = v->sname;
                tgt_ek = v->kind == TYPE_ARRAY ? v->elem_kind : ek;
            }
        } else if (tgt.kind == EXPR_FIELD) {
            std::string sn = expr_struct_name(ctx, *tgt.lhs);
            auto sit = ctx.structs.find(sn);
            if (sit != ctx.structs.end()) {
                for (const auto &f : sit->second.fields)
                    if (f.name == tgt.sval) { tgt_float = is_float_kind(f.kind); break; }
            }
        } else if (tgt.kind == EXPR_INDEX) {
            if (tgt.lhs->kind == EXPR_IDENT) {
                const CgVar *v = find_var(ctx, tgt.lhs->sval);
                if (v && v->kind == TYPE_ARRAY) {
                    tgt_float = is_float_kind(v->elem_kind);
                    tgt_agg = (v->elem_kind == TYPE_NAMED && ctx.structs.count(v->sname));
                }
            }
        }

        if (tgt_agg) {
            cgen_expr(ctx, val, offsets);
            out << "\tmov rsi, rax\n";
            cgen_lvalue_addr(ctx, tgt, offsets);
            out << "\tmov rdi, rax\n";
            int sz = 8;
            out << "\tmov rcx, " << sz << "\n";
            out << "\tcall memcpy\n";
            return !ctx.had_error;
        }

        bool is_f = cgen_expr(ctx, val, offsets);

        if (tgt.kind == EXPR_IDENT) {
            const CgVar *v = find_var(ctx, tgt.sval);
            if (!v) { cg_fail(ctx, "assign target not found: " + tgt.sval, s.line, s.col); return false; }
            emit_store_var(ctx, *v, is_f);
        } else if (tgt.kind == EXPR_INDEX) {
            // значение уже посчитано (rax/xmm0)- сохраняем его на стек, считаем адрес цели, затем достаём значение обратно и пишем по адресу
            if (is_f) out << "\tsub rsp, 8\n\tmovsd [rsp], xmm0\n";
            else out << "\tpush rax\n";
            cgen_lvalue_addr(ctx, tgt, offsets);
            out << "\tmov rcx, rax\n";
            if (is_f) { out << "\tmovsd xmm0, [rsp]\n\tadd rsp, 8\n\tmovsd [rcx], xmm0\n"; }
            else { out << "\tpop rax\n\tmov [rcx], rax\n"; }
        } else if (tgt.kind == EXPR_FIELD) {
            if (is_f) out << "\tsub rsp, 8\n\tmovsd [rsp], xmm0\n";
            else out << "\tpush rax\n";
            cgen_lvalue_addr(ctx, tgt, offsets);
            out << "\tmov rcx, rax\n";
            if (is_f) { out << "\tmovsd xmm0, [rsp]\n\tadd rsp, 8\n\tmovsd [rcx], xmm0\n"; }
            else { out << "\tpop rax\n\tmov [rcx], rax\n"; }
        }
        return !ctx.had_error;
    }

    case STMT_IF: {
        // push/pop_scope - переменные, объявленные внутри if/else, не видны после блока
        std::string lelse = ".Lelse_" + std::to_string(ctx.label_id);
        std::string lend = ".Lfi_" + std::to_string(ctx.label_id++);
        cgen_expr(ctx, *s.cond, offsets);
        out << "\ttest rax, rax\n";
        if (s.has_else) out << "\tjz " << lelse << "\n";
        else out << "\tjz " << lend << "\n";
        push_scope(ctx);
        cgen_block(ctx, s.body, offsets);
        pop_scope(ctx);
        if (s.has_else) {
            out << "\tjmp " << lend << "\n";
            out << lelse << ":\n";
            push_scope(ctx);
            cgen_block(ctx, s.else_body, offsets);
            pop_scope(ctx);
        }
        out << lend << ":\n";
        return !ctx.had_error;
    }

    case STMT_WHILE: {
        // метки текущего цикла кладём в break/cont_stack -- так break/continue
        // во вложенном цикле находят именно свои метки, а не внешнего цикла
        std::string lcond = ".Lwcond_" + std::to_string(ctx.label_id);
        std::string lend = ".Lwend_" + std::to_string(ctx.label_id++);
        ctx.break_stack.push_back(lend);
        ctx.cont_stack.push_back(lcond);
        out << lcond << ":\n";
        cgen_expr(ctx, *s.cond, offsets);
        out << "\ttest rax, rax\n\tjz " << lend << "\n";
        push_scope(ctx);
        cgen_block(ctx, s.body, offsets);
        pop_scope(ctx);
        out << "\tjmp " << lcond << "\n";
        out << lend << ":\n";
        ctx.break_stack.pop_back();
        ctx.cont_stack.pop_back();
        return !ctx.had_error;
    }

    case STMT_RETURN:
        if (s.ret_val) cgen_expr(ctx, *s.ret_val, offsets);
        out << "\tjmp " << ctx.ret_label << "\n"; // прыгаем в epilogue — один ret на всю функцию
        return !ctx.had_error;

    case STMT_BREAK:
        if (ctx.break_stack.empty()) { cg_fail(ctx, "break outside loop", s.line, s.col); return false; }
        out << "\tjmp " << ctx.break_stack.back() << "\n";
        return true;

    case STMT_CONTINUE:
        if (ctx.cont_stack.empty()) { cg_fail(ctx, "continue outside loop", s.line, s.col); return false; }
        out << "\tjmp " << ctx.cont_stack.back() << "\n";
        return true;

    case STMT_EXPR:
        cgen_expr(ctx, *s.expr, offsets);
        return !ctx.had_error;

    default:
        cg_fail(ctx, "unknown statement", s.line, s.col);
        return false;
    }
}

static bool cgen_block(CgenCtx &ctx, const Block &blk, const std::map<const Stmt *, int> &offsets) {
    for (const auto &sp : blk) {
        if (!cgen_stmt(ctx, *sp, offsets)) return false;
        if (ctx.had_error) return false;
    }
    return true;
}

static bool cgen_func(CgenCtx &ctx, const Decl &d, const std::string &ns) {
    auto &out = ctx.out;
    // A.2.8: decl_mangled хранит уже посчитанное имя с учётом перегрузок
    std::string mangled;
    auto dm = ctx.decl_mangled.find(&d);
    if (dm != ctx.decl_mangled.end()) {
        mangled = dm->second;
    } else {
        std::string full = ns.empty() ? d.name : ns + "__" + d.name;
        mangled = (ns.empty() && d.name == "main") ? "main" : "_cosmos_" + full;
    }

    // каждому параметру свой слот ниже rbp; структуры/массивы передаются по указателю (is_ptr)
    int param_off = 0;
    std::vector<std::pair<std::string, CgVar>> params;
    int ii = 0, fi = 0;
    for (const auto &p : d.params) {
        param_off -= 8;
        CgVar v = make_cgvar_from_type(param_off, p.type);
        if (v.kind == TYPE_NAMED && !ctx.structs.count(v.sname))
            v.kind = resolve_alias(ctx, v.sname);
        bool is_struct_or_arr = (v.kind == TYPE_ARRAY) ||
                                 (v.kind == TYPE_NAMED && ctx.structs.count(v.sname));
        v.is_ptr = is_struct_or_arr;
        params.push_back({p.name, v});
    }

    std::map<const Stmt *, int> stmt_offsets;
    int local_end = compute_frame(ctx, d.body, param_off, stmt_offsets);

    int frame_size = -local_end;
    frame_size = (frame_size + 15) & ~15;

    ctx.ret_label = ".Lret_" + mangled;
    ctx.ret_kind = d.ret_type.kind;

    out << "\n" << mangled << ":\n";
    out << "\tpush rbp\n";
    out << "\tmov rbp, rsp\n";
    if (frame_size > 0)
        out << "\tsub rsp, " << frame_size << "\n";

    // переносим параметры из входных регистров в их слоты на стеке — дальше с ними работаем как с обычными локальными переменными
    ii = fi = 0;
    static const char *INT_REGS[] = {"rdi","rsi","rdx","rcx","r8","r9"};
    for (const auto &[pname, pv] : params) {
        TypeKind ek = (pv.kind == TYPE_NAMED) ? resolve_alias(ctx, pv.sname) : pv.kind;
        if (is_float_kind(ek)) {
            out << "\tmovsd " << rbp_ref(pv.offset) << ", xmm" << fi << "\n";
            fi++;
        } else {
            if (ii < 6)
                out << "\tmov " << rbp_ref(pv.offset) << ", " << INT_REGS[ii] << "\n";
            ii++;
        }
    }

    push_scope(ctx);
    for (const auto &[pname, pv] : params)
        define_var(ctx, pname, pv);

    push_scope(ctx);
    cgen_block(ctx, d.body, stmt_offsets);
    pop_scope(ctx);
    pop_scope(ctx);

    out << ctx.ret_label << ":\n";
    out << "\tmov rsp, rbp\n";
    out << "\tpop rbp\n";
    out << "\tret\n";

    return !ctx.had_error;
}

static bool cgen_decls(CgenCtx &ctx, const DeclList &decls, const std::string &ns);

static bool cgen_decl(CgenCtx &ctx, const Decl &d, const std::string &ns) {
    if (d.kind == DECL_FUNC) return cgen_func(ctx, d, ns);
    if (d.kind == DECL_NAMESPACE) {
        std::string new_ns = ns.empty() ? d.name : ns + "__" + d.name;
        return cgen_decls(ctx, d.decls, new_ns);
    }
    // A.2.12: тело каждого метода генерируется как обычная функция
    // с именем _cosmos_StructName__method (self уже есть среди d.params)
    if (d.kind == DECL_IMPL)
        return cgen_decls(ctx, d.decls, d.struct_name);
    return true;
}

static bool cgen_decls(CgenCtx &ctx, const DeclList &decls, const std::string &ns) {
    for (const auto &d : decls) {
        if (!cgen_decl(ctx, *d, ns)) return false;
    }
    return true;
}

// runtime-хелперы: panic, str_concat, input — компилируются прямо в .s, внешней библиотеки нет
static void emit_preamble(std::ostream &out) {
    out << "# Cosmos compiler output — GAS x86-64 Intel syntax Linux\n";
    out << "# Сборка: gcc out.s -o out -no-pie\n\n";
    out << ".intel_syntax noprefix\n";
    out << ".section .note.GNU-stack,\"\",@progbits\n\n";

    out << ".extern printf\n";
    out << ".extern fgets\n";
    out << ".extern strlen\n";
    out << ".extern strcmp\n";
    out << ".extern malloc\n";
    out << ".extern free\n";
    out << ".extern memcpy\n";
    out << ".extern exit\n";
    out << ".extern stdin\n\n";

    out << ".global main\n\n";

    out << ".text\n\n";

    // panic(msg) -- пишет "panic: <msg>\n" в stderr (fd 2) сырыми write-сисколами и завершает
    // процесс через exit(1) (syscall 60); printf не используем, чтобы не зависеть от его буфера
    out << "_cosmos_panic:\n";
    out << "\tpush rbp\n\tmov rbp, rsp\n";
    out << "\tsub rsp, 16\n";
    out << "\tmov [rbp-8], rdi\n";
    out << "\tlea rsi, _err_prefix[rip]\n";
    out << "\tmov rdx, 16\n";
    out << "\tmov rdi, 2\n";
    out << "\tmov rax, 1\n\tsyscall\n";
    out << "\tmov rdi, [rbp-8]\n";
    out << "\tcall strlen\n";
    out << "\tmov rdx, rax\n";
    out << "\tmov rsi, [rbp-8]\n";
    out << "\tmov rdi, 2\n";
    out << "\tmov rax, 1\n\tsyscall\n";
    out << "\tmov rdi, 2\n";
    out << "\tlea rsi, _err_nl[rip]\n";
    out << "\tmov rdx, 1\n";
    out << "\tmov rax, 1\n\tsyscall\n";
    out << "\tmov rdi, 1\n\tmov rax, 60\n\tsyscall\n";
    out << "\tpop rbp\n\tret\n\n";

    // a + b для строк: выделяем malloc(len(a)+len(b)+1), копируем обе части и '\0' в конце
    out << "_cosmos_str_concat:\n";
    out << "\tpush rbp\n\tmov rbp, rsp\n\tsub rsp, 32\n";
    out << "\tmov [rbp-8],  rdi\n";
    out << "\tmov [rbp-16], rsi\n";
    out << "\tcall strlen\n";
    out << "\tmov [rbp-24], rax\n";
    out << "\tmov rdi, [rbp-16]\n\tcall strlen\n";
    out << "\tmov [rbp-32], rax\n";
    out << "\tmov rdi, [rbp-24]\n\tadd rdi, [rbp-32]\n\tinc rdi\n";
    out << "\tcall malloc\n";
    out << "\tmov r12, rax\n";
    out << "\tmov rdi, r12\n";
    out << "\tmov rsi, [rbp-8]\n";
    out << "\tmov rdx, [rbp-24]\n";
    out << "\tcall memcpy\n";
    out << "\tmov rdi, r12\n\tadd rdi, [rbp-24]\n";
    out << "\tmov rsi, [rbp-16]\n";
    out << "\tmov rdx, [rbp-32]\n\tinc rdx\n";
    out << "\tcall memcpy\n";
    out << "\tmov rax, r12\n";
    out << "\tmov rsp, rbp\n\tpop rbp\n\tret\n\n";

    // input() -- читает строку из stdin в буфер на стеке (4096 байт), срезает
    // завершающий '\n' если он есть, копирует результат в malloc'нутую строку
    out << "_cosmos_input:\n";
    out << "\tpush rbp\n\tmov rbp, rsp\n\tsub rsp, 4112\n";
    out << "\tlea rdi, [rbp-4112]\n";
    out << "\tmov rsi, 4096\n";
    out << "\tmov rdx, stdin[rip]\n";
    out << "\tcall fgets\n";
    out << "\tlea rdi, [rbp-4112]\n\tcall strlen\n";
    out << "\ttest rax, rax\n\tjz .input_done\n";
    out << "\tlea rcx, [rbp-4112]\n";
    out << "\tadd rcx, rax\n\tdec rcx\n";
    out << "\tcmp byte ptr [rcx], 10\n\tjne .input_done\n";
    out << "\tmov byte ptr [rcx], 0\n";
    out << ".input_done:\n";
    out << "\tlea rdi, [rbp-4112]\n\tcall strlen\n";
    out << "\tinc rax\n\tmov rdi, rax\n\tcall malloc\n";
    out << "\tmov rdi, rax\n";
    out << "\tlea rsi, [rbp-4112]\n";
    out << "\tlea rcx, [rbp-4112]\n\tcall strlen\n\tinc rax\n\tmov rdx, rax\n";
    out << "\tcall memcpy\n";
    out << "\tmov rsp, rbp\n\tpop rbp\n\tret\n\n";

    // print() без аргументов это просто перевод строки
    out << "_cosmos_print_nl:\n";
    out << "\tlea rdi, _fmt_nl[rip]\n\txor al, al\n\tcall printf\n\tret\n\n";
}

static void emit_data(std::ostream &out, const std::vector<std::string> &strlits, const std::vector<double> &flitvals) {
    out << "\n.section .rodata\n\n";

    for (int i = 0; i < (int)strlits.size(); i++) {
        out << "_str_" << i << ": .byte ";
        for (unsigned char c : strlits[i]) {
            out << (int)c << ",";
        }
        out << "0\n";
    }

    for (int i = 0; i < (int)flitvals.size(); i++) {
        double v = flitvals[i];
        uint64_t bits;
        static_assert(sizeof(v) == sizeof(bits));
        __builtin_memcpy(&bits, &v, 8);
        char buf[32];
        snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)bits);
        out << "_flt_" << i << ": .quad 0x" << buf << "\n"; // IEEE 754 double как 8 байт
    }

    out << "\n_fmt_int:       .asciz \"%ld\\n\"\n";
    out << "_fmt_uint:      .asciz \"%lu\\n\"\n";
    out << "_fmt_float:     .asciz \"%g\\n\"\n";
    out << "_fmt_str:       .asciz \"%s\\n\"\n";
    out << "_fmt_str_plain: .asciz \"%s\"\n";
    out << "_fmt_nl:        .asciz \"\\n\"\n";
    out << "_fmt_arr_open:  .asciz \"[\"\n";
    out << "_fmt_arr_sep:   .asciz \", \"\n";
    out << "_fmt_arr_close: .asciz \"]\\n\"\n";

    out << "_str_true:  .asciz \"true\"\n";
    out << "_str_false: .asciz \"false\"\n";
    out << "_str_empty: .byte 0\n";

    out << "_err_prefix:  .asciz \"runtime error: \"\n";
    out << "_err_divzero: .asciz \"division by zero\"\n";
    out << "_err_bounds:  .asciz \"index out of bounds\"\n";
    out << "_err_nl: .byte 10, 0\n";
}

bool codegen(const Program *prog, const std::string &asm_path, CodegenError *err) {
    std::ofstream f(asm_path);
    if (!f) {
        *err = {"cannot open output file: " + asm_path, 0, 0};
        return false;
    }

    CgenCtx ctx{f};

    collect_info(ctx, prog->decls, "");
    emit_preamble(f); // runtime + .text
    cgen_decls(ctx, prog->decls, "");
    emit_data(f, ctx.strlits, ctx.flitvals); // .rodata: строки и float-константы собранные во время обхода

    if (ctx.had_error) {
        *err = ctx.err;
        return false;
    }
    return true;
}

#include "codegen.hpp"
#include "lexer.hpp"
#include "optimize.hpp"
#include "parser.hpp"
#include "semantic.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

static std::string read_file(const std::string &path) {
    std::ifstream f(path);
    if (!f) { std::cerr << "error: cannot open '" << path << "'\n"; std::exit(1); }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string read_stdin(bool interactive) {
    if (interactive)
        std::cerr << "cosmos -- enter source, end with Ctrl+D\n\n";
    std::string src, line;
    while (std::getline(std::cin, line)) { src += line; src += '\n'; }
    return src;
}

static const char *tok_type_str(TokenType t) {
    switch (t) {
    case TOKEN_INT: 
        return "INT";
    case TOKEN_FLOAT:
        return "FLOAT";
    case TOKEN_BOOL:
        return "BOOL";
    case TOKEN_STRING:
        return "STRING";
    case TOKEN_IDENT:   
        return "IDENT";
    case TOKEN_KW:      
        return "KW";
    case TOKEN_OP:      
        return "OP";
    case TOKEN_NEWLINE: 
        return "NEWLINE";
    case TOKEN_INDENT:  
        return "INDENT";
    case TOKEN_DEDENT:  
        return "DEDENT";
    case TOKEN_EOF:     
        return "EOF";
    case TOKEN_ERROR:   
        return "ERROR";
    }
    return "?";
}

int main(int argc, char *argv[]) {
    std::string path;
    bool flag_tokens = false;
    bool flag_ast = false;
    bool flag_tree = false;
    bool flag_emit_asm = false;
    bool flag_time_phases = false;
    bool flag_optimize = false;
    std::string out_path;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--tokens")
            flag_tokens = true;
        else if (arg == "--ast")
            flag_ast = true;
        else if (arg == "--tree")
            flag_tree = true;
        else if (arg == "--emit-asm")
            flag_emit_asm = true;
        else if (arg == "--time-phases")
            flag_time_phases = true;
        else if (arg == "--optimize")
            flag_optimize = true;
        else if (arg == "-o" && i + 1 < argc)
            out_path = argv[++i];
        else if (arg[0] != '-')
            path = arg;
        else {
            std::cerr << "unknown option '" << arg << "'\n";
            return 1;
        }
    }

    bool from_stdin = path.empty();
    bool interactive = from_stdin && isatty(STDIN_FILENO);
    if (interactive && !flag_tokens && !flag_ast) flag_tree = true;

    std::string src = from_stdin ? read_stdin(interactive) : read_file(path);
    std::string filename = from_stdin ? "<stdin>" : path;

    using clock_t = std::chrono::steady_clock;
    auto report_phase = [&](const char *name, clock_t::time_point start) {
        if (flag_time_phases) {
            double ms = std::chrono::duration<double, std::milli>(clock_t::now() - start).count();
            std::cerr << name << ": " << ms << " ms\n";
        }
    };

    // lex
    auto t_lex = clock_t::now();
    LexError lex_err{};
    std::vector<Token> *tokens = tokenize(src.c_str(), &lex_err);
    report_phase("lexer", t_lex);
    if (!tokens) {
        std::cerr << filename << ":" << lex_err.line << ":" << lex_err.col << ": error: " << lex_err.message << "\n";
        return 1;
    }

    if (flag_tokens) {
        for (const auto &t : *tokens)
            std::cout << t.line << ":" << t.col << "\t" << tok_type_str(t.type) << "\t\"" << t.lexeme << "\"\n";
        tokens_free(tokens);
        return 0;
    }

    // parse
    auto t_parse = clock_t::now();
    Program prog;
    ParseError parse_err{};
    bool parse_ok = parse(tokens, &prog, &parse_err);
    report_phase("parser", t_parse);
    if (!parse_ok) {
        std::cerr << filename << ":" << parse_err.line << ":" << parse_err.col << ": error: " << parse_err.message << "\n";
        tokens_free(tokens);
        return 1;
    }
    tokens_free(tokens);

    // semantic check
    auto t_sem = clock_t::now();
    SemError sem_err{};
    bool sem_ok = sem_check(&prog, &sem_err);
    report_phase("semantic", t_sem);
    if (!sem_ok) {
        std::cerr << filename << ":" << sem_err.line << ":" << sem_err.col << ": error: " << sem_err.message << "\n";
        return 1;
    }

    // оптимизации на уровне AST 
    // показывают дерево уже после оптимизаций.
    if (flag_optimize) {
        auto t_opt = clock_t::now();
        optimize_program(&prog);
        report_phase("optimize", t_opt);
    }

    if (flag_tree) {
        print_tree(&prog, std::cout); 
        return 0; 
    }
    if (flag_ast) {
        dump_ast(&prog, std::cout);   
        return 0;
     }

    // Кодоген: пишем ассемблер
    std::string asm_file = out_path;
    if (asm_file.empty()) {
        // Заменяем расширение .csm → .s, или добавляем .s
        if (!path.empty()) {
            asm_file = path;
            auto dot = asm_file.rfind('.');
            if (dot != std::string::npos) asm_file = asm_file.substr(0, dot);
            asm_file += ".s";
        } else {
            asm_file = "out.s";
        }
    }

    auto t_codegen = clock_t::now();
    CodegenError cg_err{};
    bool cg_ok = codegen(&prog, asm_file, &cg_err);
    report_phase("codegen", t_codegen);
    if (!cg_ok) {
        std::cerr << filename << ":" << cg_err.line << ":" << cg_err.col << ": error: " << cg_err.message << "\n";
        return 1;
    }

    if (!flag_emit_asm) {
        std::cerr << "assembly written to " << asm_file << "\n";
        std::cerr << "to compile: gcc " << asm_file << " -o out -no-pie\n";
    }
    return 0;
}

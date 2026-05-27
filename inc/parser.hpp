#pragma once

#include "ast.hpp"
#include <ostream>
#include <vector>

typedef struct {
    std::string message;
    int line, col;
} ParseError;

bool parse(const std::vector<Token> *tokens, Program *out, ParseError *err);
void dump_ast(const Program *prog, std::ostream &out);
void print_tree(const Program *prog, std::ostream &out);

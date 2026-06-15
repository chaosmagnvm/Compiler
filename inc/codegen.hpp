#pragma once

#include "ast.hpp"
#include <string>

typedef struct {
    std::string message;
    int line, col;
} CodegenError;

//   nasm -f elf64 out.s -o out.o && gcc out.o -o out -no-pie
bool codegen(const Program *prog, const std::string &asm_path, CodegenError *err);

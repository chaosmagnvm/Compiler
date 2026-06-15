#pragma once

#include "ast.hpp"
#include <string>

typedef struct {
    std::string message;
    int line, col;
} SemError;

bool sem_check(const Program *prog, SemError *err);

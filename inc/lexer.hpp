#pragma once

#include "token.hpp"
#include <vector>

typedef struct {
    std::string message;
    int line;
    int col;
} LexError;

std::vector<Token> *tokenize(const char *src, LexError *err);
void tokens_free(std::vector<Token> *tokens);

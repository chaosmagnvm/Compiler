#include "lexer.hpp"

#include <cctype>
#include <cstring>
#include <stack>
#include <string>
#include <vector>

typedef struct {
    const char *word;
    KeywordType kw;
} KwEntry; // ключевое слово - строка и тип

static const KwEntry KEYWORDS[KEYWORDS_COUNT] = {
    {"and", KW_AND},
    {"array", KW_ARRAY},
    {"as", KW_AS},
    {"bool", KW_BOOL},
    {"break", KW_BREAK},
    {"continue", KW_CONTINUE},
    {"def", KW_DEF},
    {"else", KW_ELSE},
    {"exit", KW_EXIT},
    {"false", KW_FALSE},
    {"float32", KW_FLOAT32},
    {"float64", KW_FLOAT64},
    {"if", KW_IF},
    {"impl", KW_IMPL},
    {"input", KW_INPUT},
    {"int16", KW_INT16},
    {"int32", KW_INT32},
    {"int64", KW_INT64},
    {"int8", KW_INT8},
    {"len", KW_LEN},
    {"let", KW_LET},
    {"namespace", KW_NAMESPACE},
    {"not", KW_NOT},
    {"or", KW_OR},
    {"panic", KW_PANIC},
    {"print", KW_PRINT},
    {"return", KW_RETURN},
    {"sizeof", KW_SIZEOF},
    {"string", KW_STRING},
    {"struct", KW_STRUCT},
    {"true", KW_TRUE},
    {"type", KW_TYPE},
    {"typeof", KW_TYPEOF},
    {"uint16", KW_UINT16},
    {"uint32", KW_UINT32},
    {"uint64", KW_UINT64},
    {"uint8", KW_UINT8},
    {"var", KW_VAR},
    {"void", KW_VOID},
    {"while", KW_WHILE},
};

static KeywordType kw_lookup(const char *word, int len) { // бинарный поиск по массиву ключевых слов
    int i = 0, n = KEYWORDS_COUNT - 1;
    while (i <= n) {
        int mid = (i + n) / 2;
        int cmp = strncmp(KEYWORDS[mid].word, word, len);
        if (cmp == 0 && (int)strlen(KEYWORDS[mid].word) == len)
            return KEYWORDS[mid].kw;
        if (cmp < 0 || (cmp == 0 && (int)strlen(KEYWORDS[mid].word) < len))
            i = mid + 1;
        else
            n = mid - 1;
    }
    return KW_NONE;
}

typedef struct { const char *sym; int len; OpType op; } OpEntry; // оператор - строка, длина и тип

static const OpEntry OPERATORS[OPERATORS_COUNT] = {
    {"==", 2, OP_EQEQ},
    {"!=", 2, OP_NEQ},
    {"<=", 2, OP_LEQ},
    {">=", 2, OP_GEQ},
    {"->", 2, OP_ARROW},
    {"::", 2, OP_DCOLON},
    {"+", 1, OP_PLUS},
    {"-", 1, OP_MINUS},
    {"*", 1, OP_STAR},
    {"/", 1, OP_SLASH},
    {"%", 1, OP_PERCENT},
    {"<", 1, OP_LT},
    {">", 1, OP_GT},
    {"=", 1, OP_EQ},
    {":", 1, OP_COLON},
    {",", 1, OP_COMMA},
    {".", 1, OP_DOT},
    {"(", 1, OP_LPAREN},
    {")", 1, OP_RPAREN},
    {"[", 1, OP_LBRACK},
    {"]", 1, OP_RBRACK},
    {"{", 1, OP_LBRACE},
    {"}", 1, OP_RBRACE},
};

static Token make_token(TokenType type, const char *str, int len, int line, int col) { // создает токен
    Token t;
    t.lexeme = std::string(str, len);
    t.type = type;
    t.kw = KW_NONE;
    t.op = OP_NONE;
    t.line = line;
    t.col = col;
    return t;
}

static Token make_kw_token(KeywordType kw, const char *str, int len, int line, int col) {
    Token t = make_token(TOKEN_KW, str, len, line, col);
    t.kw = kw;
    return t;
}

static Token make_op_token(OpType op, const char *str, int len, int line, int col) {
    Token t = make_token(TOKEN_OP, str, len, line, col);
    t.op = op;
    return t;
}

static Token make_simple_token(TokenType type, int line, int col) {
    return make_token(type, "", 0, line, col);
}

static Token scan_string(const char *src, int *i, int line, int col) {
    (*i)++; // сканирует строковый литерал
    std::string val;
    while (src[*i] && src[*i] != '"') {
        if (src[*i] == '\\') {
            (*i)++;
            switch (src[*i]) {
            case '"':  val += '"';  break;
            case '\\': val += '\\'; break;
            case 'n':  val += '\n'; break;
            case 't':  val += '\t'; break;
            default: {
                Token e = make_simple_token(TOKEN_ERROR, line, col);
                e.lexeme = std::string("unknown escape '\\") + src[*i] + "'";
                return e;
            }
            }
        } else {
            val += src[*i];
        }
        (*i)++;
    }
    if (!src[*i]) {
        Token e = make_simple_token(TOKEN_ERROR, line, col);
        e.lexeme = "unterminated string literal";
        return e;
    }
    (*i)++;
    return make_token(TOKEN_STRING, val.c_str(), (int)val.size(), line, col);
}

static Token scan_number(const char *src, int *i, int line, int col) {
    int start = *i;
    while (isdigit((unsigned char)src[*i]))
        (*i)++;
    bool is_float = (src[*i] == '.' && isdigit((unsigned char)src[*i + 1]));
    if (is_float) {
        (*i)++;
        while (isdigit((unsigned char)src[*i]))
            (*i)++;
    }
    return make_token(is_float ? TOKEN_FLOAT : TOKEN_INT, src + start, *i - start, line, col);
}

static Token scan_ident(const char *src, int *i, int line, int col) {
    int start = *i;
    while (isalnum((unsigned char)src[*i]) || src[*i] == '_')
        (*i)++;
    int len = *i - start;
    KeywordType kw = kw_lookup(src + start, len);
    if (kw != KW_NONE)
        return make_kw_token(kw, src + start, len, line, col);
    return make_token(TOKEN_IDENT, src + start, len, line, col);
}

static Token scan_op(const char *src, int *i, int line, int col) {
    for (int k = 0; k < OPERATORS_COUNT; k++) {
        int len = OPERATORS[k].len;
        if (strncmp(src + *i, OPERATORS[k].sym, len) == 0) {
            Token t = make_op_token(OPERATORS[k].op, src + *i, len, line, col);
            *i += len;
            return t;
        }
    }
    Token e = make_simple_token(TOKEN_ERROR, line, col);
    e.lexeme = std::string("unexpected character '") + src[*i] + "'";
    (*i)++;
    return e;
}

// Заменяет блочные комментарии /* ... */ (с поддержкой вложенности) пробелами
// и переводами строк той же длины, чтобы позиции токенов после комментария не сдвигались.
// Строковые литералы и однострочные # -комментарии копируются как есть, без обработки.
static std::string strip_block_comments(const char *src, LexError *err) {
    std::string out;
    int line = 1, col = 1;
    int i = 0;
    while (src[i]) {
        char c = src[i];
        if (c == '"') {
            out += c;
            i++; col++;
            while (src[i] && src[i] != '"') {
                if (src[i] == '\\' && src[i + 1]) {
                    out += src[i]; out += src[i + 1];
                    i += 2; col += 2;
                    continue;
                }
                out += src[i];
                i++; col++;
            }
            if (src[i] == '"') { out += src[i]; i++; col++; }
            continue;
        }
        if (c == '#') {
            while (src[i] && src[i] != '\n') { out += src[i]; i++; col++; }
            continue;
        }
        if (c == '/' && src[i + 1] == '*') {
            int start_line = line, start_col = col;
            int depth = 1;
            out += "  ";
            i += 2; col += 2;
            while (depth > 0) {
                if (!src[i]) {
                    err->message = "unterminated block comment";
                    err->line = start_line;
                    err->col = start_col;
                    return std::string();
                }
                if (src[i] == '/' && src[i + 1] == '*') {
                    depth++;
                    out += "  ";
                    i += 2; col += 2;
                } else if (src[i] == '*' && src[i + 1] == '/') {
                    depth--;
                    out += "  ";
                    i += 2; col += 2;
                } else if (src[i] == '\n') {
                    out += '\n';
                    i++; line++; col = 1;
                } else {
                    out += ' ';
                    i++; col++;
                }
            }
            continue;
        }
        out += c;
        if (c == '\n') { line++; col = 1; } else col++;
        i++;
    }
    return out;
}

std::vector<Token> *tokenize(const char *src, LexError *err) {
    std::string stripped = strip_block_comments(src, err);
    if (!err->message.empty())
        return nullptr;
    src = stripped.c_str();

    auto *tokens = new std::vector<Token>();
    std::stack<int> indent_stack; // считаем отступы
    indent_stack.push(0);

    int i = 0;
    int line = 1;

    while (src[i]) {
        int line_start = i;
        int indent = 0;
        while (src[i] == ' ') { i++; indent++; }

        if (src[i] == '\n' || src[i] == '#' || src[i] == '\0') {
            while (src[i] && src[i] != '\n') i++;
            if (src[i] == '\n') { i++; line++; }
            continue;
        }

        if (src[line_start] == '\t') {
            err->message = "tabs not allowed for indentation";
            err->line = line;
            err->col = 1;
            delete tokens;
            return nullptr;
        }

        int cur = indent_stack.top();
        if (indent > cur) {
            tokens->push_back(make_simple_token(TOKEN_INDENT, line, 1));
            indent_stack.push(indent);
        } else if (indent < cur) {
            while (indent_stack.top() > indent) {
                tokens->push_back(make_simple_token(TOKEN_DEDENT, line, 1));
                indent_stack.pop();
            }
            if (indent_stack.top() != indent) {
                err->message = "unindent does not match any outer indentation level";
                err->line = line;
                err->col = 1;
                delete tokens;
                return nullptr;
            }
        }

        int col = indent + 1;
        while (src[i] && src[i] != '\n') {
            if (src[i] == ' ')  { i++; col++; continue; }
            if (src[i] == '#')  { while (src[i] && src[i] != '\n') i++; break; }

            Token t;
            if (src[i] == '"')
                t = scan_string(src, &i, line, col);
            else if (isdigit((unsigned char)src[i]))
                t = scan_number(src, &i, line, col);
            else if (isalpha((unsigned char)src[i]) || src[i] == '_')
                t = scan_ident(src, &i, line, col);
            else
                t = scan_op(src, &i, line, col);

            if (t.type == TOKEN_ERROR) {
                err->message = t.lexeme;
                err->line = t.line;
                err->col = t.col;
                delete tokens;
                return nullptr;
            }

            col += (int)t.lexeme.size();
            tokens->push_back(std::move(t));
        }

        tokens->push_back(make_simple_token(TOKEN_NEWLINE, line, col));
        if (src[i] == '\n') { i++; line++; }
    }

    while (indent_stack.top() > 0) {
        tokens->push_back(make_simple_token(TOKEN_DEDENT, line, 1));
        indent_stack.pop();
    }

    tokens->push_back(make_simple_token(TOKEN_EOF, line, 1));
    return tokens;
}

void tokens_free(std::vector<Token> *tokens) {
    delete tokens;
}

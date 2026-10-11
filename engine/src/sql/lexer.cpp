#include "querylab/sql/lexer.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace querylab::sql {

namespace {

constexpr std::array kKeywords = {
    "ALL",     "AND",     "AS",      "ASC",     "BETWEEN", "BIGINT", "BOOL",    "BOOLEAN", "BY",
    "CREATE",  "DELETE",  "DESC",    "DISTINCT", "DOUBLE", "DROP",   "EXISTS",  "EXPLAIN", "FALSE",
    "FLOAT",   "FROM",    "GROUP",   "HAVING",  "IF",      "IN",     "INDEX",   "INNER",   "INSERT",
    "INT",     "INTEGER", "INTO",    "IS",      "JOIN",    "LEFT",   "LIKE",    "LIMIT",   "NOT",
    "NULL",    "OFFSET",  "ON",      "OR",      "ORDER",   "OUTER",  "REAL",    "SELECT",  "SET",
    "TABLE",   "TEXT",    "TRUE",    "UNIQUE",  "UPDATE",  "VALUES", "VARCHAR", "WHERE",
};

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

std::string upper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
    return out;
}
std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

class Lexer {
public:
    Lexer(std::string_view src, bool keepComments) : src_(src), keepComments_(keepComments) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            skipWhitespace();
            if (pos_ >= src_.size()) break;
            const std::size_t start = pos_;
            const int line = line_, col = col_;
            Token t = next();
            t.span = SourceSpan{start, pos_, line, col};
            t.lexeme = std::string(src_.substr(start, pos_ - start));
            if (t.type == TokenType::Comment && !keepComments_) continue;
            out.push_back(std::move(t));
        }
        Token end;
        end.type = TokenType::End;
        end.span = SourceSpan{pos_, pos_, line_, col_};
        out.push_back(end);
        return out;
    }

private:
    char peek(std::size_t k = 0) const { return pos_ + k < src_.size() ? src_[pos_ + k] : '\0'; }

    void advance() {
        if (src_[pos_] == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        ++pos_;
    }

    void skipWhitespace() {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) advance();
    }

    [[noreturn]] void fail(const std::string& msg, std::size_t start, int line, int col) const {
        throw QueryError(ErrorStage::Lexer, msg, SourceSpan{start, std::max(pos_, start + 1), line, col});
    }

    Token next() {
        const std::size_t start = pos_;
        const int line = line_, col = col_;
        const char c = peek();
        Token t;

        if (c == '-' && peek(1) == '-') {
            while (pos_ < src_.size() && peek() != '\n') advance();
            t.type = TokenType::Comment;
            return t;
        }
        if (c == '/' && peek(1) == '*') {
            advance();
            advance();
            while (true) {
                if (pos_ >= src_.size()) fail("unterminated block comment", start, line, col);
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    break;
                }
                advance();
            }
            t.type = TokenType::Comment;
            return t;
        }
        if (isIdentStart(c)) {
            while (isIdentChar(peek())) advance();
            const std::string_view word = src_.substr(start, pos_ - start);
            const std::string up = upper(word);
            if (isKeyword(up)) {
                t.type = TokenType::Keyword;
                t.value = up;
            } else {
                t.type = TokenType::Identifier;
                t.value = lower(word);
            }
            return t;
        }
        if (c == '"') {
            advance();
            std::string value;
            while (true) {
                if (pos_ >= src_.size()) fail("unterminated quoted identifier", start, line, col);
                if (peek() == '"') {
                    if (peek(1) == '"') {
                        value += '"';
                        advance();
                        advance();
                        continue;
                    }
                    advance();
                    break;
                }
                value += peek();
                advance();
            }
            if (value.empty()) fail("empty quoted identifier", start, line, col);
            t.type = TokenType::Identifier;
            t.value = value;
            return t;
        }
        if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
            bool isFloat = false;
            while (isDigit(peek())) advance();
            if (peek() == '.') {
                isFloat = true;
                advance();
                while (isDigit(peek())) advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                const char sign = peek(1);
                if (isDigit(sign) || ((sign == '+' || sign == '-') && isDigit(peek(2)))) {
                    isFloat = true;
                    advance();
                    if (!isDigit(peek())) advance();
                    while (isDigit(peek())) advance();
                } else {
                    advance();
                    fail("malformed exponent in numeric literal", start, line, col);
                }
            }
            if (isIdentStart(peek())) {
                advance();
                fail("invalid character in numeric literal", start, line, col);
            }
            t.type = isFloat ? TokenType::Float : TokenType::Integer;
            t.value = std::string(src_.substr(start, pos_ - start));
            return t;
        }
        if (c == '\'') {
            advance();
            std::string value;
            while (true) {
                if (pos_ >= src_.size()) fail("unterminated string literal", start, line, col);
                if (peek() == '\'') {
                    if (peek(1) == '\'') {  // SQL escape: '' -> '
                        value += '\'';
                        advance();
                        advance();
                        continue;
                    }
                    advance();
                    break;
                }
                value += peek();
                advance();
            }
            t.type = TokenType::String;
            t.value = std::move(value);
            return t;
        }

        // Multi-character operators first.
        static constexpr std::array<std::string_view, 5> kTwoChar = {"<=", ">=", "<>", "!=", "||"};
        for (auto op : kTwoChar) {
            if (src_.substr(pos_, 2) == op) {
                advance();
                advance();
                t.type = TokenType::Operator;
                t.value = op == "!=" ? "<>" : std::string(op);
                return t;
            }
        }
        switch (c) {
            case '=': case '<': case '>': case '+': case '-': case '*': case '/': case '%':
                advance();
                t.type = TokenType::Operator;
                t.value = std::string(1, c);
                return t;
            case '(': case ')': case ',': case ';': case '.':
                advance();
                t.type = TokenType::Punctuation;
                t.value = std::string(1, c);
                return t;
            default:
                break;
        }
        advance();
        std::string shown = std::isprint(static_cast<unsigned char>(c)) ? std::string(1, c)
                                                                        : "byte 0x" + std::to_string(+static_cast<unsigned char>(c));
        fail("unexpected character '" + shown + "'", start, line, col);
    }

    std::string_view src_;
    bool keepComments_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
};

}  // namespace

const char* tokenTypeName(TokenType t) {
    switch (t) {
        case TokenType::Keyword: return "keyword";
        case TokenType::Identifier: return "identifier";
        case TokenType::Integer: return "integer";
        case TokenType::Float: return "float";
        case TokenType::String: return "string";
        case TokenType::Operator: return "operator";
        case TokenType::Punctuation: return "punctuation";
        case TokenType::Comment: return "comment";
        case TokenType::End: return "end";
    }
    return "?";
}

bool isKeyword(std::string_view up) {
    return std::find(kKeywords.begin(), kKeywords.end(), up) != kKeywords.end();
}

std::vector<Token> tokenize(std::string_view sql, bool keepComments) {
    return Lexer(sql, keepComments).run();
}

}  // namespace querylab::sql

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "querylab/common/error.h"

namespace querylab::sql {

enum class TokenType { Keyword, Identifier, Integer, Float, String, Operator, Punctuation, Comment, End };

const char* tokenTypeName(TokenType t);

struct Token {
    TokenType type = TokenType::End;
    /// Exact source text of the token.
    std::string lexeme;
    /// Normalized value: upper-cased keyword, identifier (lower-cased unless
    /// it was double-quoted), unescaped string literal contents, or operator.
    std::string value;
    SourceSpan span;
};

/// Converts SQL text into tokens. Throws QueryError(Lexer) for malformed input
/// such as unterminated strings/comments or unexpected characters. Comments
/// are only included when `keepComments` is true (for the token viewer).
std::vector<Token> tokenize(std::string_view sql, bool keepComments = false);

bool isKeyword(std::string_view upper);

}  // namespace querylab::sql

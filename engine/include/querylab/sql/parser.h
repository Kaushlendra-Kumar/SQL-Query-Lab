#pragma once

#include <string_view>
#include <vector>

#include "querylab/sql/ast.h"
#include "querylab/sql/lexer.h"

namespace querylab::sql {

/// Parses one or more ';'-separated statements. Throws QueryError(Lexer|Parser)
/// with the source location of the offending token.
std::vector<Statement> parseScript(std::string_view sql);

/// Parses exactly one statement (a trailing ';' is allowed).
Statement parseStatement(std::string_view sql);

/// Parses a standalone expression (used by tests).
ExprPtr parseExpression(std::string_view sql);

}  // namespace querylab::sql

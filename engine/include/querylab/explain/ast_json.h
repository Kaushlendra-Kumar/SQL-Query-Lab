#pragma once

#include <nlohmann/json.hpp>

#include "querylab/sql/ast.h"
#include "querylab/sql/lexer.h"

namespace querylab::explain {

using json = nlohmann::json;

json spanToJson(const SourceSpan& s);
json valueToJson(const Value& v);

/// Serializes a token stream: [{type, lexeme, value, span}].
json tokensToJson(const std::vector<sql::Token>& tokens);

/// Serializes a parsed statement into a tree of
/// {id, type, label, span, details, children} nodes. Clause nodes (SelectList,
/// From, Where, ...) group their contents so the viewer mirrors SQL structure.
json statementToJson(const sql::Statement& stmt);

/// ASCII tree rendering of the same structure, for the CLI.
std::string astToText(const json& node);

}  // namespace querylab::explain

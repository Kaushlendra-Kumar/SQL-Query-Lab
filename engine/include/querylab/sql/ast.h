#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "querylab/common/error.h"
#include "querylab/common/value.h"

namespace querylab::sql {

enum class ExprKind {
    Literal,    // 42, 'x', TRUE, NULL
    ColumnRef,  // [table.]column
    Star,       // * or t.*   (only valid in SELECT list / COUNT(*))
    Unary,      // op: "NOT", "-", "+"
    Binary,     // op: AND OR = <> < <= > >= + - * / % ||
    IsNull,     // children[0] IS [NOT] NULL   (negated => IS NOT NULL)
    Between,    // children: value, low, high  (negated => NOT BETWEEN)
    InList,     // children: value, item...    (negated => NOT IN)
    Like,       // children: value, pattern    (negated => NOT LIKE)
    Function,   // name(args) / COUNT(*) / COUNT(DISTINCT x)
};

const char* exprKindName(ExprKind k);

/// A single node type keeps the parser, binder and AST serializer simple.
/// Only the fields relevant to `kind` are meaningful.
struct Expr {
    ExprKind kind = ExprKind::Literal;
    SourceSpan span;
    Value literal;                 // Literal
    std::string table;             // ColumnRef / Star qualifier (may be empty)
    std::string column;            // ColumnRef
    std::string op;                // Unary / Binary
    std::string name;              // Function (lower-case)
    bool negated = false;          // IsNull / Between / InList / Like
    bool distinct = false;         // Function: COUNT(DISTINCT x)
    bool starArg = false;          // Function: COUNT(*)
    std::vector<std::unique_ptr<Expr>> children;

    /// SQL-like rendering used for labels, default column names and plans.
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::unique_ptr<Expr> clone() const;
};

using ExprPtr = std::unique_ptr<Expr>;

struct TableRef {
    std::string name;
    std::string alias;  // empty if none
    SourceSpan span;
    [[nodiscard]] const std::string& visibleName() const { return alias.empty() ? name : alias; }
};

enum class JoinType { Inner, Left };

struct JoinClause {
    JoinType type = JoinType::Inner;
    TableRef table;
    ExprPtr on;
    SourceSpan span;
};

struct SelectItem {
    ExprPtr expr;
    std::string alias;
    SourceSpan span;
};

struct OrderItem {
    ExprPtr expr;
    bool descending = false;
    SourceSpan span;
};

struct SelectStmt {
    bool distinct = false;
    std::vector<SelectItem> items;
    std::optional<TableRef> from;
    std::vector<JoinClause> joins;
    ExprPtr where;
    std::vector<ExprPtr> groupBy;
    ExprPtr having;
    std::vector<OrderItem> orderBy;
    std::optional<std::int64_t> limit;
    std::optional<std::int64_t> offset;
    SourceSpan limitSpan, offsetSpan;
};

struct ColumnDef {
    std::string name;
    DataType type = DataType::Integer;
    SourceSpan span;
};

struct CreateTableStmt {
    std::string name;
    bool ifNotExists = false;
    std::vector<ColumnDef> columns;
    SourceSpan nameSpan;
};

struct CreateIndexStmt {
    std::string name;
    std::string table;
    std::vector<std::string> columns;
    std::vector<SourceSpan> columnSpans;
    bool unique = false;
    bool ifNotExists = false;
    SourceSpan tableSpan;
};

struct DropTableStmt {
    std::string name;
    bool ifExists = false;
    SourceSpan nameSpan;
};

struct DropIndexStmt {
    std::string name;
    bool ifExists = false;
    SourceSpan nameSpan;
};

struct InsertStmt {
    std::string table;
    SourceSpan tableSpan;
    std::vector<std::string> columns;  // empty => all columns in order
    std::vector<SourceSpan> columnSpans;
    std::vector<std::vector<ExprPtr>> rows;
};

struct Assignment {
    std::string column;
    SourceSpan span;
    ExprPtr value;
};

struct UpdateStmt {
    std::string table;
    SourceSpan tableSpan;
    std::vector<Assignment> assignments;
    ExprPtr where;
};

struct DeleteStmt {
    std::string table;
    SourceSpan tableSpan;
    ExprPtr where;
};

using StatementBody = std::variant<SelectStmt, CreateTableStmt, CreateIndexStmt, DropTableStmt, DropIndexStmt,
                                   InsertStmt, UpdateStmt, DeleteStmt>;

struct Statement {
    StatementBody body;
    bool explain = false;  // EXPLAIN prefix
    SourceSpan span;
    std::string text;      // exact source text of this statement

    [[nodiscard]] const char* kindName() const;
    [[nodiscard]] bool isSelect() const { return std::holds_alternative<SelectStmt>(body); }
};

}  // namespace querylab::sql

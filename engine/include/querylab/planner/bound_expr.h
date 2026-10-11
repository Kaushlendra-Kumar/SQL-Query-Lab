#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "querylab/catalog/catalog.h"
#include "querylab/common/error.h"
#include "querylab/common/value.h"

namespace querylab::planner {

/// Every column flowing through a plan has a query-wide unique id. Bound
/// expressions refer to columns by id, never by name; physical planning then
/// maps ids to row positions ("slots") once, so nothing is resolved by string
/// at run time.
using ColumnId = int;

struct ColumnMeta {
    ColumnId id = -1;
    std::string name;       // output / column name
    std::string qualifier;  // range variable (alias or table) for base columns; empty otherwise
    DataType type = DataType::Integer;
    /// Base-table origin, used for statistics and the index advisor.
    const catalog::TableInfo* sourceTable = nullptr;
    int sourceColumn = -1;

    [[nodiscard]] std::string displayName() const { return qualifier.empty() ? name : qualifier + "." + name; }
};
using Schema = std::vector<ColumnMeta>;

enum class BoundKind { Literal, Column, Unary, Binary, IsNull, Between, InList, Like, Function };

struct BoundExpr {
    BoundKind kind = BoundKind::Literal;
    DataType type = DataType::Null;
    Value literal;
    ColumnId column = -1;
    std::string columnName;  // display only
    int slot = -1;           // resolved row position (physical plans only)
    std::string op;          // Unary/Binary operator, upper-case for NOT/AND/OR
    std::string func;        // Function name (lower-case)
    bool negated = false;
    SourceSpan span;
    std::vector<std::unique_ptr<BoundExpr>> children;

    [[nodiscard]] std::unique_ptr<BoundExpr> clone() const;
    [[nodiscard]] std::string toString() const;
    /// Canonical structural key (columns by id); equal keys => equal expressions.
    [[nodiscard]] std::string key() const;
    void collectColumns(std::set<ColumnId>& out) const;
    [[nodiscard]] bool isConstant() const;  // no column references
};

using BoundExprPtr = std::unique_ptr<BoundExpr>;

BoundExprPtr makeLiteral(Value v);
BoundExprPtr makeColumn(const ColumnMeta& c);
BoundExprPtr makeBinary(std::string op, BoundExprPtr l, BoundExprPtr r);  // AND/OR/comparisons (boolean result)

/// Splits a predicate into its AND-ed conjuncts (consumes the input).
std::vector<BoundExprPtr> splitConjuncts(BoundExprPtr e);
/// Re-joins conjuncts with AND (nullptr if empty).
BoundExprPtr combineConjuncts(std::vector<BoundExprPtr> parts);

/// Maps column ids to slots using `layout`. Throws std::logic_error if a
/// referenced column is not produced by the input (planner bug).
void resolveSlots(BoundExpr& e, const Schema& layout);

/// Evaluates with SQL semantics (three-valued logic; NULL propagation).
/// Throws QueryError(Executor) for runtime errors (division by zero, overflow).
Value evaluate(const BoundExpr& e, const Row& row);

/// Three-valued truth: true only when the result is TRUE (WHERE semantics).
inline bool isTrue(const Value& v) { return v.type() == DataType::Boolean && v.asBool(); }

/// SQL LIKE with % and _ (case-sensitive).
bool likeMatch(const std::string& text, const std::string& pattern);

/// Is `name` an aggregate function (count/sum/avg/min/max)?
bool isAggregateName(const std::string& name);

}  // namespace querylab::planner

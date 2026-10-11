#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "querylab/planner/bound_expr.h"
#include "querylab/sql/ast.h"

namespace querylab::planner {

enum class LogicalOp { Scan, Filter, Project, Join, Aggregate, Sort, Limit, Distinct, Values, Empty };
const char* logicalOpName(LogicalOp op);

struct AggregateCall {
    std::string func;   // count, sum, avg, min, max
    BoundExprPtr arg;   // nullptr for COUNT(*)
    bool distinct = false;
    ColumnMeta output;  // the column this aggregate produces
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] AggregateCall clone() const;
};

struct SortKey {
    BoundExprPtr expr;
    bool descending = false;
};

/// Cardinality / cost estimates attached by the cost model.
struct Estimates {
    double rows = 0;       // estimated output rows
    double selfCost = 0;   // cost of this operator alone (abstract units)
    double totalCost = 0;  // selfCost + children's totalCost
};

/// A relational-algebra operator. Fields are used according to `op`:
///   Scan:      table, alias, scanColumns (table positions read), predicate (pushed-down filter)
///   Filter:    predicate
///   Project:   exprs (one per output column)
///   Join:      joinType, predicate (join condition, may be null => cross join)
///   Aggregate: groupBy (first outputs), aggregates (remaining outputs)
///   Sort:      sortKeys
///   Limit:     limit / offset
///   Distinct:  (none)
///   Values:    one row with no columns (SELECT without FROM)
///   Empty:     produces no rows (e.g. WHERE FALSE)
struct LogicalNode {
    int id = 0;
    LogicalOp op = LogicalOp::Values;
    std::vector<std::unique_ptr<LogicalNode>> children;
    Schema output;

    const catalog::TableInfo* table = nullptr;
    std::string alias;
    std::vector<std::size_t> scanColumns;
    BoundExprPtr predicate;
    sql::JoinType joinType = sql::JoinType::Inner;
    std::vector<BoundExprPtr> exprs;
    std::vector<BoundExprPtr> groupBy;
    std::vector<AggregateCall> aggregates;
    std::vector<SortKey> sortKeys;
    std::optional<std::int64_t> limit;
    std::int64_t offset = 0;

    Estimates est;
    /// Optimizer decisions that affected this node (shown in the inspector).
    std::vector<std::string> notes;

    [[nodiscard]] std::unique_ptr<LogicalNode> clone() const;
    /// One-line description, e.g. "Filter(age > 25)".
    [[nodiscard]] std::string label() const;
    /// Indented tree rendering for the CLI and tests.
    [[nodiscard]] std::string toTreeString() const;
};

using LogicalPtr = std::unique_ptr<LogicalNode>;

/// Hands out unique node and column ids for one query.
struct IdGenerator {
    int nextNode = 1;
    ColumnId nextColumn = 1;
};

}  // namespace querylab::planner

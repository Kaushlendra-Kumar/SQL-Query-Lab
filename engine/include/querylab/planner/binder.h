#pragma once

#include <map>
#include <string>
#include <vector>

#include "querylab/catalog/catalog.h"
#include "querylab/planner/logical_plan.h"
#include "querylab/sql/ast.h"

namespace querylab::planner {

/// Semantic analysis: resolves tables, columns, aliases, functions and types,
/// and translates a SELECT into an (unoptimized) logical plan.
///
/// The plan is the canonical relational translation:
///   Limit → Sort → Distinct → Project → [Filter(HAVING) → Aggregate] →
///   Filter(WHERE) → Join tree → Scans
/// The optimizer is responsible for pushing filters down, pruning columns,
/// reordering joins, etc.
class Binder {
public:
    Binder(const catalog::Catalog& catalog, IdGenerator& ids) : catalog_(catalog), ids_(ids) {}

    LogicalPtr bindSelect(const sql::SelectStmt& stmt);

    /// For UPDATE/DELETE: Filter(where) over Scan(table) exposing all columns.
    LogicalPtr bindTableFilter(const std::string& table, const SourceSpan& tableSpan, const sql::Expr* where);

    /// Binds an expression against an existing schema (UPDATE ... SET).
    BoundExprPtr bindAgainst(const sql::Expr& e, const Schema& schema, const std::string& qualifier);

    /// Evaluates a constant expression (INSERT VALUES). Column references and
    /// aggregates are rejected.
    Value evalConstant(const sql::Expr& e);

    /// Number of node ids handed out, for diagnostics.
    [[nodiscard]] IdGenerator& ids() { return ids_; }

private:
    struct RangeVar {
        std::string visible;
        const catalog::TableInfo* table = nullptr;
        Schema columns;
    };
    struct Scope {
        std::vector<const RangeVar*> vars;
    };
    struct AggContext {
        const Scope* base = nullptr;
        std::vector<std::pair<std::string, ColumnMeta>> groups;  // key -> output column
        std::vector<AggregateCall>* aggregates = nullptr;
        bool allowAggregates = true;
    };

    RangeVar makeRangeVar(const sql::TableRef& ref);
    LogicalPtr scanNode(const RangeVar& rv);
    LogicalPtr newNode(LogicalOp op);

    BoundExprPtr bindExpr(const sql::Expr& e, const Scope& scope, AggContext* agg, const char* clause);
    BoundExprPtr bindNode(const sql::Expr& e, const Scope& scope, AggContext* agg, const char* clause);
    BoundExprPtr bindColumn(const sql::Expr& e, const Scope& scope);
    BoundExprPtr bindAggregateCall(const sql::Expr& e, const Scope& scope, AggContext& agg);
    void requireBoolean(const BoundExpr& e, const char* clause, const SourceSpan& span);

    static bool containsAggregate(const sql::Expr& e);
    ColumnMeta newColumn(std::string name, DataType type, const BoundExpr* source);

    const catalog::Catalog& catalog_;
    IdGenerator& ids_;
    std::vector<std::unique_ptr<RangeVar>> rangeVars_;
    std::map<ColumnId, ColumnMeta> columnMeta_;
};

}  // namespace querylab::planner

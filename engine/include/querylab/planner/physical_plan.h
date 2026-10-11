#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "querylab/optimizer/cost_model.h"
#include "querylab/optimizer/optimizer.h"
#include "querylab/planner/logical_plan.h"

namespace querylab::planner {

enum class PhysicalOp {
    SeqScan,
    IndexScan,
    Filter,
    Project,
    NestedLoopJoin,
    HashJoin,
    Sort,
    Limit,
    HashAggregate,
    HashDistinct,
    Values,
    Empty,
    Update,
    Delete,
};
const char* physicalOpName(PhysicalOp op);

/// Key range for an index scan: equality on the first `eqPrefix.size()`
/// index columns, then an optional range on the next column.
struct IndexBounds {
    const catalog::IndexInfo* index = nullptr;
    Row eqPrefix;
    std::optional<Value> lo, hi;
    bool loInclusive = true, hiInclusive = true;
    std::vector<std::string> usedPredicates;  // conjuncts answered by the index
    [[nodiscard]] std::string describe(const catalog::TableInfo& t) const;
};

struct PhysicalNode {
    int id = 0;
    int logicalId = 0;  // logical node this implements
    PhysicalOp op = PhysicalOp::Values;
    std::vector<std::unique_ptr<PhysicalNode>> children;
    Schema output;

    // Scans
    const catalog::TableInfo* table = nullptr;
    std::string alias;
    std::vector<std::size_t> scanColumns;  // table positions emitted, in output order
    BoundExprPtr predicate;                // scan filter / Filter predicate / join residual
    IndexBounds bounds;

    // Joins: HashJoin probes children[0] and builds a hash table on children[1].
    sql::JoinType joinType = sql::JoinType::Inner;
    std::vector<BoundExprPtr> leftKeys, rightKeys;

    // Project / aggregate / sort / limit
    std::vector<BoundExprPtr> exprs;
    std::vector<BoundExprPtr> groupBy;
    std::vector<AggregateCall> aggregates;
    std::vector<SortKey> sortKeys;
    std::optional<std::int64_t> limit;
    std::int64_t offset = 0;

    // UPDATE: (column position, new value expression over the scanned row)
    std::vector<std::pair<std::size_t, BoundExprPtr>> assignments;

    Estimates est;
    std::vector<std::string> notes;

    [[nodiscard]] std::string label() const;
    [[nodiscard]] std::string toTreeString() const;
};

using PhysicalPtr = std::unique_ptr<PhysicalNode>;

/// Turns an optimized logical plan into physical operators: chooses scan
/// methods (sequential vs index), join algorithms and hash-join build sides,
/// and resolves column ids to row slots. Records each decision, with the
/// costed alternatives, in the optimizer trace.
class PhysicalPlanner {
public:
    PhysicalPlanner(const optimizer::CostModel& cost, std::vector<optimizer::OptimizerStep>& trace)
        : cost_(cost), trace_(trace) {}

    PhysicalPtr plan(const LogicalNode& root);

    /// Wraps a scan plan for UPDATE/DELETE.
    PhysicalPtr planDelete(const LogicalNode& root);
    PhysicalPtr planUpdate(const LogicalNode& root, std::vector<std::pair<std::size_t, BoundExprPtr>> assignments);

    /// Disable index scans (for "what if" comparisons in the performance lab).
    void setIndexScansEnabled(bool on) { indexScans_ = on; }
    void setHashJoinEnabled(bool on) { hashJoin_ = on; }

private:
    PhysicalPtr build(const LogicalNode& n);
    PhysicalPtr buildScan(const LogicalNode& n);
    PhysicalPtr buildJoin(const LogicalNode& n);
    void finishCost(PhysicalNode& n);

    const optimizer::CostModel& cost_;
    std::vector<optimizer::OptimizerStep>& trace_;
    int nextId_ = 1;
    bool indexScans_ = true;
    bool hashJoin_ = true;
};

/// Schema describing a full table row for a scan, with ids placed at the
/// table positions that are read (others are -1). Used to resolve scan
/// predicates against decoded rows.
Schema tableLayout(const PhysicalNode& scan);

}  // namespace querylab::planner

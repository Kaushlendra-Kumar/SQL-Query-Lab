#pragma once

#include <map>
#include <string>

#include "querylab/planner/logical_plan.h"

namespace querylab::optimizer {

using planner::BoundExpr;
using planner::ColumnId;
using planner::ColumnMeta;
using planner::LogicalNode;

/// Tunable constants of the cost model, in abstract "cost units" where reading
/// one page sequentially costs 1.0. These are teaching heuristics modelled on
/// PostgreSQL's planner constants, not calibrated predictions of run time.
struct CostParams {
    double seqPageCost = 1.0;        // read one heap page during a sequential scan
    double randomPageCost = 1.5;     // fetch a distinct heap page through an index
    double bufferLookupCost = 0.02;  // per-row heap fetch through the buffer pool
    double indexNodeCost = 0.25;     // visit one B+ tree node
    double indexEntryCost = 0.005;   // examine one index entry
    double cpuTupleCost = 0.01;      // produce/decode one row
    double cpuOperatorCost = 0.0025; // evaluate one predicate / expression
    double hashBuildCost = 0.02;     // insert one row into a hash table
    double hashProbeCost = 0.01;     // probe the hash table once
    double sortCompareCost = 0.005;  // one comparison while sorting
    double defaultSelectivity = 1.0 / 3.0;
};

/// Cardinality and cost estimation shared by the optimizer and the physical
/// planner so their numbers agree.
class CostModel {
public:
    explicit CostModel(CostParams p = {}) : p_(p) {}

    /// Registers the columns produced anywhere in `root` so predicates can be
    /// mapped back to base-table statistics.
    void indexColumns(const LogicalNode& root);
    void registerColumn(const ColumnMeta& c) { columns_[c.id] = c; }

    /// Estimated fraction of input rows for which `pred` is TRUE.
    [[nodiscard]] double selectivity(const BoundExpr& pred) const;
    /// Estimated distinct values of a column (falls back to `fallbackRows`).
    [[nodiscard]] double distinctValues(ColumnId c, double fallbackRows) const;
    /// Estimated output rows of a join of `left` x `right` rows under `cond`.
    [[nodiscard]] double joinRows(double left, double right, const BoundExpr* cond, bool leftOuter) const;

    /// Fills `est` on every node, assuming default implementations
    /// (sequential scans, hash join for equi-joins, hash aggregation).
    void annotate(LogicalNode& node) const;

    // Cost formulas (also used for physical alternatives).
    [[nodiscard]] double seqScanCost(double pages, double rows, std::size_t predicateTerms, double columnFraction = 1.0) const;
    [[nodiscard]] double indexScanCost(double treeHeight, double matched, double heapPages, std::size_t residualTerms,
                                       double columnFraction = 1.0) const;
    [[nodiscard]] double hashJoinCost(double buildRows, double probeRows, double outRows) const;
    [[nodiscard]] double nestedLoopCost(double outerRows, double innerRows, double outRows) const;
    [[nodiscard]] double sortCost(double rows, std::size_t keys) const;
    [[nodiscard]] double hashAggCost(double rows, std::size_t groups, std::size_t aggs) const;

    [[nodiscard]] const CostParams& params() const { return p_; }
    [[nodiscard]] const ColumnMeta* column(ColumnId id) const;

    /// Explains how a selectivity was derived (for the inspector), e.g.
    /// "age > 25: histogram range estimate 0.41".
    [[nodiscard]] std::string explainSelectivity(const BoundExpr& pred) const;

private:
    const catalog::ColumnStats* statsFor(ColumnId c, double* tableRows) const;
    double comparisonSelectivity(const std::string& op, ColumnId col, const Value& v) const;
    double rangeSelectivity(ColumnId col, const Value* lo, bool loInc, const Value* hi, bool hiInc) const;

    CostParams p_;
    std::map<ColumnId, ColumnMeta> columns_;
};

/// Number of AND-ed terms in a predicate (0 for nullptr).
std::size_t conjunctCount(const BoundExpr* e);

/// A `left.col = right.col` conjunct usable as a hash-join key.
struct EquiKey {
    ColumnId left = -1;
    ColumnId right = -1;
};
/// Extracts equi-join keys from the AND-ed conjuncts of `cond`, oriented so
/// `left` comes from `leftSchema` and `right` from `rightSchema`.
std::vector<EquiKey> equiKeys(const BoundExpr* cond, const planner::Schema& leftSchema, const planner::Schema& rightSchema);

}  // namespace querylab::optimizer

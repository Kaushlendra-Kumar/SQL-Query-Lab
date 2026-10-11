#pragma once

#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "querylab/catalog/catalog.h"
#include "querylab/execution/executor.h"
#include "querylab/optimizer/cost_model.h"
#include "querylab/optimizer/optimizer.h"
#include "querylab/planner/physical_plan.h"
#include "querylab/sql/ast.h"

namespace querylab::explain {

using json = nlohmann::json;

struct InsightInput {
    const sql::Statement* statement = nullptr;
    const planner::PhysicalNode* plan = nullptr;
    const std::map<int, execution::OperatorStats>* stats = nullptr;  // null for EXPLAIN only
    const std::vector<optimizer::OptimizerStep>* trace = nullptr;
    const optimizer::CostModel* cost = nullptr;
    const catalog::Catalog* catalog = nullptr;
    double executeMs = 0;
    std::uint64_t resultRows = 0;
    double timerOverheadNs = 0;
    std::uint64_t timerCalls = 0;
    int queryFrequency = 1;  // how many times this query shape ran in the session
};

/// Plain-language explanation built only from engine metadata and
/// measurements. Returns {summary, facts[], guidance[], estimateWarnings[], hotspot}.
/// `facts` are observations of this execution; `guidance` is general
/// educational material and is labelled as such.
json explainExecution(const InsightInput& in);

/// Index suggestions grounded in the plan and its runtime statistics.
/// Each suggestion: {table, columns, definition, evidence, expectedBenefit,
/// overhead, limitations, verify, estimatedCost{current, withIndex}}.
json adviseIndexes(const InsightInput& in);

}  // namespace querylab::explain

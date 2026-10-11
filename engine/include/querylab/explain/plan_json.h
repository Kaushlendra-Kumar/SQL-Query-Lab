#pragma once

#include <map>

#include <nlohmann/json.hpp>

#include "querylab/execution/executor.h"
#include "querylab/optimizer/cost_model.h"
#include "querylab/optimizer/optimizer.h"
#include "querylab/planner/logical_plan.h"
#include "querylab/planner/physical_plan.h"

namespace querylab::explain {

using json = nlohmann::json;

json schemaToJson(const planner::Schema& s);
json logicalToJson(const planner::LogicalNode& n);
/// `stats` may be null (EXPLAIN without execution).
json physicalToJson(const planner::PhysicalNode& n, const std::map<int, execution::OperatorStats>* stats,
                    const optimizer::CostModel& cost);
json optimizerTraceToJson(const std::vector<optimizer::OptimizerStep>& steps);

/// Short beginner-friendly description of what an operator does.
const char* describePhysicalOp(planner::PhysicalOp op);
const char* describeLogicalOp(planner::LogicalOp op);

}  // namespace querylab::explain

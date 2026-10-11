#pragma once

#include <string>
#include <vector>

#include "querylab/optimizer/cost_model.h"
#include "querylab/planner/logical_plan.h"

namespace querylab::optimizer {

/// A competing implementation considered by a physical decision.
struct Alternative {
    std::string name;
    double cost = 0;
    bool chosen = false;
    std::string detail;
};

/// One entry of the optimizer trace shown in the UI.
struct OptimizerStep {
    std::string rule;
    std::string phase;  // "logical" | "physical"
    bool applied = false;
    std::string reason;
    std::vector<std::string> details;
    planner::LogicalPtr before;  // logical rules only, when applied
    planner::LogicalPtr after;
    double costBefore = 0;
    double costAfter = 0;
    std::vector<Alternative> alternatives;  // physical decisions
    std::vector<int> nodeIds;               // nodes affected
};

/// Rule-based logical optimizer. Every rule is semantics-preserving; when a
/// rule cannot be applied safely it leaves the plan unchanged and records why.
///
/// Rules, in order: ConstantFolding, FilterSimplification, PredicatePushdown,
/// JoinReordering, ProjectionPushdown.
class Optimizer {
public:
    Optimizer(CostModel& cost, planner::IdGenerator& ids) : cost_(cost), ids_(ids) {}

    planner::LogicalPtr optimize(planner::LogicalPtr plan, std::vector<OptimizerStep>& trace);

    struct RuleResult {
        bool applied = false;
        std::string reason;
        std::vector<std::string> details;
        std::vector<int> nodeIds;
    };

    // Individual rules (public for tests).
    RuleResult constantFolding(planner::LogicalPtr& plan);
    RuleResult filterSimplification(planner::LogicalPtr& plan);
    RuleResult predicatePushdown(planner::LogicalPtr& plan);
    RuleResult joinReordering(planner::LogicalPtr& plan);
    RuleResult projectionPushdown(planner::LogicalPtr& plan);

private:
    CostModel& cost_;
    planner::IdGenerator& ids_;
};

}  // namespace querylab::optimizer

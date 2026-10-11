#include "querylab/optimizer/optimizer.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <limits>
#include <set>

namespace querylab::optimizer {

using planner::BoundExpr;
using planner::BoundExprPtr;
using planner::BoundKind;
using planner::LogicalNode;
using planner::LogicalOp;
using planner::LogicalPtr;
using planner::Schema;

namespace {

std::string fmtCost(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

/// Applies fn to every expression slot in the node (not children).
void forEachExpr(LogicalNode& n, const std::function<void(BoundExprPtr&)>& fn) {
    if (n.predicate) fn(n.predicate);
    for (auto& e : n.exprs) fn(e);
    for (auto& e : n.groupBy) fn(e);
    for (auto& a : n.aggregates) {
        if (a.arg) fn(a.arg);
    }
    for (auto& k : n.sortKeys) fn(k.expr);
}

void forEachNode(LogicalNode& n, const std::function<void(LogicalNode&)>& fn) {
    fn(n);
    for (auto& c : n.children) forEachNode(*c, fn);
}

std::set<planner::ColumnId> columnsOf(const BoundExpr& e) {
    std::set<planner::ColumnId> s;
    e.collectColumns(s);
    return s;
}

std::set<planner::ColumnId> idsOf(const Schema& s) {
    std::set<planner::ColumnId> out;
    for (const auto& c : s) out.insert(c.id);
    return out;
}

bool subset(const std::set<planner::ColumnId>& a, const std::set<planner::ColumnId>& b) {
    return std::includes(b.begin(), b.end(), a.begin(), a.end());
}

bool isBoolLiteral(const BoundExpr& e, bool v) {
    return e.kind == BoundKind::Literal && e.literal.type() == DataType::Boolean && e.literal.asBool() == v;
}
bool isNullLiteral(const BoundExpr& e) { return e.kind == BoundKind::Literal && e.literal.isNull(); }

// ---- constant folding -------------------------------------------------------

void fold(BoundExprPtr& e, std::vector<std::string>& log) {
    for (auto& c : e->children) fold(c, log);
    if (e->kind == BoundKind::Literal || e->kind == BoundKind::Column || !e->isConstant()) return;
    try {
        Value v = planner::evaluate(*e, Row{});
        const std::string before = e->toString();
        const DataType t = e->type;
        const SourceSpan span = e->span;
        e = planner::makeLiteral(std::move(v));
        if (e->type == DataType::Null) e->type = t;
        e->span = span;
        log.push_back(before + " → " + e->toString());
    } catch (const QueryError&) {
        // Unsafe to fold (e.g. division by zero): leave it for run time so
        // the error is raised only if the expression is actually evaluated.
    }
}

// ---- filter simplification ----------------------------------------------------

void simplify(BoundExprPtr& e, std::vector<std::string>& log) {
    for (auto& c : e->children) simplify(c, log);
    if (e->kind == BoundKind::Binary && (e->op == "AND" || e->op == "OR")) {
        const bool isAnd = e->op == "AND";
        for (int side = 0; side < 2; ++side) {
            auto& self = e->children[static_cast<std::size_t>(side)];
            auto& other = e->children[static_cast<std::size_t>(1 - side)];
            // x AND TRUE = x;  x OR FALSE = x  (holds for NULL x too)
            if (isBoolLiteral(*self, isAnd)) {
                const std::string before = e->toString();
                BoundExprPtr keep = std::move(other);
                e = std::move(keep);
                log.push_back(before + " → " + e->toString());
                return;
            }
            // x AND FALSE = FALSE;  x OR TRUE = TRUE  (holds for NULL x too)
            if (isBoolLiteral(*self, !isAnd)) {
                const std::string before = e->toString();
                BoundExprPtr keep = std::move(self);
                e = std::move(keep);
                log.push_back(before + " → " + e->toString());
                return;
            }
        }
    }
    // NOT NOT x = x under three-valued logic.
    if (e->kind == BoundKind::Unary && e->op == "NOT" && e->children[0]->kind == BoundKind::Unary && e->children[0]->op == "NOT") {
        const std::string before = e->toString();
        BoundExprPtr inner = std::move(e->children[0]->children[0]);
        e = std::move(inner);
        log.push_back(before + " → " + e->toString());
    }
}

LogicalPtr makeEmpty(planner::IdGenerator& ids, const Schema& output) {
    auto n = std::make_unique<LogicalNode>();
    n->id = ids.nextNode++;
    n->op = LogicalOp::Empty;
    n->output = output;
    return n;
}

/// Removes always-true filters, replaces always-false subtrees with Empty and
/// propagates Empty upward where that is semantics-preserving.
void prune(LogicalPtr& n, planner::IdGenerator& ids, std::vector<std::string>& log, std::vector<int>& nodes) {
    for (auto& c : n->children) prune(c, ids, log, nodes);
    auto alwaysTrue = [](const BoundExprPtr& p) { return p && isBoolLiteral(*p, true); };
    auto neverTrue = [](const BoundExprPtr& p) { return p && (isBoolLiteral(*p, false) || isNullLiteral(*p)); };

    if (n->op == LogicalOp::Filter && alwaysTrue(n->predicate)) {
        log.push_back("removed Filter(TRUE)");
        nodes.push_back(n->id);
        LogicalPtr child = std::move(n->children[0]);
        n = std::move(child);
        return;
    }
    if ((n->op == LogicalOp::Filter || n->op == LogicalOp::Scan) && neverTrue(n->predicate)) {
        log.push_back(std::string(planner::logicalOpName(n->op)) + " predicate can never be TRUE; replaced subtree with Empty");
        nodes.push_back(n->id);
        n = makeEmpty(ids, n->output);
        return;
    }
    if (n->op == LogicalOp::Scan && alwaysTrue(n->predicate)) {
        n->predicate.reset();
        log.push_back("removed always-true scan predicate");
    }
    if (n->op == LogicalOp::Join && n->joinType == sql::JoinType::Inner && neverTrue(n->predicate)) {
        log.push_back("inner join condition can never be TRUE; replaced with Empty");
        n = makeEmpty(ids, n->output);
        return;
    }
    // Empty propagation.
    const bool anyEmpty = std::any_of(n->children.begin(), n->children.end(), [](const LogicalPtr& c) { return c->op == LogicalOp::Empty; });
    if (!anyEmpty) return;
    bool becomesEmpty = false;
    switch (n->op) {
        case LogicalOp::Filter:
        case LogicalOp::Project:
        case LogicalOp::Sort:
        case LogicalOp::Limit:
        case LogicalOp::Distinct: becomesEmpty = true; break;
        case LogicalOp::Join:
            becomesEmpty = n->joinType == sql::JoinType::Inner || n->children[0]->op == LogicalOp::Empty;
            break;
        case LogicalOp::Aggregate: becomesEmpty = !n->groupBy.empty(); break;  // global aggregates still return 1 row
        default: break;
    }
    if (becomesEmpty) {
        log.push_back(std::string(planner::logicalOpName(n->op)) + " over an empty input is empty");
        n = makeEmpty(ids, n->output);
    }
}

// ---- predicate pushdown ----------------------------------------------------------

struct PushedPred {
    BoundExprPtr expr;
    bool fromWhere = true;
};

struct PushdownState {
    planner::IdGenerator& ids;
    std::vector<std::string>& log;
    std::vector<int>& nodes;
};

LogicalPtr wrapFilter(LogicalPtr child, std::vector<PushedPred> preds, PushdownState& st) {
    if (preds.empty()) return child;
    std::vector<BoundExprPtr> parts;
    for (auto& p : preds) parts.push_back(std::move(p.expr));
    auto f = std::make_unique<LogicalNode>();
    f->id = st.ids.nextNode++;
    f->op = LogicalOp::Filter;
    f->predicate = planner::combineConjuncts(std::move(parts));
    f->output = child->output;
    f->children.push_back(std::move(child));
    return f;
}

std::string nodeName(const LogicalNode& n) {
    if (n.op == LogicalOp::Scan) return "Scan(" + n.alias + ")";
    return planner::logicalOpName(n.op);
}

LogicalPtr pushDown(LogicalPtr n, std::vector<PushedPred> preds, PushdownState& st) {
    switch (n->op) {
        case LogicalOp::Filter: {
            for (auto& p : planner::splitConjuncts(std::move(n->predicate))) preds.push_back({std::move(p), true});
            return pushDown(std::move(n->children[0]), std::move(preds), st);
        }
        case LogicalOp::Scan: {
            if (preds.empty()) return n;
            std::vector<BoundExprPtr> parts;
            if (n->predicate) parts.push_back(std::move(n->predicate));
            for (auto& p : preds) {
                st.log.push_back("pushed " + p.expr->toString() + " into " + nodeName(*n) + " (evaluated while reading pages)");
                parts.push_back(std::move(p.expr));
            }
            st.nodes.push_back(n->id);
            n->predicate = planner::combineConjuncts(std::move(parts));
            return n;
        }
        case LogicalOp::Join: {
            const auto leftIds = idsOf(n->children[0]->output);
            const auto rightIds = idsOf(n->children[1]->output);
            std::vector<PushedPred> toLeft, toRight, above;
            std::vector<BoundExprPtr> joinConds;
            const bool inner = n->joinType == sql::JoinType::Inner;
            // Conjuncts from the ON clause.
            for (auto& p : planner::splitConjuncts(std::move(n->predicate))) {
                const auto cols = columnsOf(*p);
                if (!cols.empty() && subset(cols, rightIds)) {
                    st.log.push_back("pushed ON condition " + p->toString() + " below the join into its right input");
                    toRight.push_back({std::move(p), false});
                } else if (inner && !cols.empty() && subset(cols, leftIds)) {
                    st.log.push_back("pushed ON condition " + p->toString() + " below the join into its left input");
                    toLeft.push_back({std::move(p), false});
                } else {
                    joinConds.push_back(std::move(p));
                }
            }
            // Conjuncts from above (WHERE).
            for (auto& p : preds) {
                const auto cols = columnsOf(*p.expr);
                if (!cols.empty() && subset(cols, leftIds)) {
                    toLeft.push_back(std::move(p));
                } else if (inner && !cols.empty() && subset(cols, rightIds)) {
                    toRight.push_back(std::move(p));
                } else if (inner && !cols.empty()) {
                    st.log.push_back("merged WHERE condition " + p.expr->toString() + " into the join condition");
                    st.nodes.push_back(n->id);
                    joinConds.push_back(std::move(p.expr));
                } else {
                    // LEFT JOIN: filtering the null-extended side above the
                    // join is not equivalent to filtering below it.
                    above.push_back(std::move(p));
                }
            }
            n->predicate = planner::combineConjuncts(std::move(joinConds));
            n->children[0] = pushDown(std::move(n->children[0]), std::move(toLeft), st);
            n->children[1] = pushDown(std::move(n->children[1]), std::move(toRight), st);
            n->output = n->children[0]->output;
            n->output.insert(n->output.end(), n->children[1]->output.begin(), n->children[1]->output.end());
            return wrapFilter(std::move(n), std::move(above), st);
        }
        default: {
            for (auto& c : n->children) c = pushDown(std::move(c), {}, st);
            return wrapFilter(std::move(n), std::move(preds), st);
        }
    }
}

// ---- join reordering -------------------------------------------------------------

bool isInnerJoinTree(const LogicalNode& n) { return n.op == LogicalOp::Join && n.joinType == sql::JoinType::Inner; }

void collectJoinLeaves(LogicalPtr n, std::vector<LogicalPtr>& leaves, std::vector<BoundExprPtr>& conds) {
    if (isInnerJoinTree(*n)) {
        for (auto& p : planner::splitConjuncts(std::move(n->predicate))) conds.push_back(std::move(p));
        collectJoinLeaves(std::move(n->children[0]), leaves, conds);
        collectJoinLeaves(std::move(n->children[1]), leaves, conds);
        return;
    }
    leaves.push_back(std::move(n));
}

std::string leafName(const LogicalNode& n) {
    if (n.op == LogicalOp::Scan) return n.alias;
    if (n.op == LogicalOp::Filter && !n.children.empty()) return leafName(*n.children[0]);
    return std::string(planner::logicalOpName(n.op)) + "#" + std::to_string(n.id);
}

// ---- projection pushdown ---------------------------------------------------------

void requireExpr(const BoundExpr* e, std::set<planner::ColumnId>& req) {
    if (e) e->collectColumns(req);
}

void prunePlan(LogicalNode& n, std::set<planner::ColumnId> req, std::vector<std::string>& log, std::vector<int>& nodes) {
    switch (n.op) {
        case LogicalOp::Scan: {
            requireExpr(n.predicate.get(), req);
            std::vector<std::size_t> keepCols;
            Schema keepOut;
            for (std::size_t i = 0; i < n.output.size(); ++i) {
                if (req.count(n.output[i].id)) {
                    keepCols.push_back(n.scanColumns[i]);
                    keepOut.push_back(n.output[i]);
                }
            }
            if (keepOut.size() < n.output.size()) {
                std::string cols;
                for (const auto& c : keepOut) cols += (cols.empty() ? "" : ", ") + c.name;
                log.push_back(n.alias + ": reads " + std::to_string(keepOut.size()) + " of " + std::to_string(n.table->columns.size()) +
                              " columns" + (cols.empty() ? " (row count only)" : " (" + cols + ")"));
                nodes.push_back(n.id);
                n.scanColumns = std::move(keepCols);
                n.output = std::move(keepOut);
            }
            return;
        }
        case LogicalOp::Filter:
            requireExpr(n.predicate.get(), req);
            prunePlan(*n.children[0], req, log, nodes);
            n.output = n.children[0]->output;
            return;
        case LogicalOp::Project: {
            std::set<planner::ColumnId> childReq;
            for (const auto& e : n.exprs) requireExpr(e.get(), childReq);
            prunePlan(*n.children[0], childReq, log, nodes);
            return;
        }
        case LogicalOp::Sort:
            for (const auto& k : n.sortKeys) requireExpr(k.expr.get(), req);
            prunePlan(*n.children[0], req, log, nodes);
            n.output = n.children[0]->output;
            return;
        case LogicalOp::Limit:
            prunePlan(*n.children[0], req, log, nodes);
            n.output = n.children[0]->output;
            return;
        case LogicalOp::Distinct:
            // DISTINCT compares whole rows: every input column is required.
            prunePlan(*n.children[0], idsOf(n.children[0]->output), log, nodes);
            return;
        case LogicalOp::Aggregate: {
            std::set<planner::ColumnId> childReq;
            for (const auto& g : n.groupBy) requireExpr(g.get(), childReq);
            for (const auto& a : n.aggregates) requireExpr(a.arg.get(), childReq);
            prunePlan(*n.children[0], childReq, log, nodes);
            return;
        }
        case LogicalOp::Join: {
            requireExpr(n.predicate.get(), req);
            prunePlan(*n.children[0], req, log, nodes);
            prunePlan(*n.children[1], req, log, nodes);
            n.output = n.children[0]->output;
            n.output.insert(n.output.end(), n.children[1]->output.begin(), n.children[1]->output.end());
            return;
        }
        default:
            for (auto& c : n.children) prunePlan(*c, req, log, nodes);
            return;
    }
}

}  // namespace

// ---- rules ---------------------------------------------------------------------------

Optimizer::RuleResult Optimizer::constantFolding(LogicalPtr& plan) {
    RuleResult r;
    forEachNode(*plan, [&](LogicalNode& n) {
        const auto before = r.details.size();
        forEachExpr(n, [&](BoundExprPtr& e) { fold(e, r.details); });
        if (r.details.size() > before) r.nodeIds.push_back(n.id);
    });
    r.applied = !r.details.empty();
    r.reason = r.applied ? "Evaluated constant sub-expressions once at plan time instead of once per row."
                         : "No constant sub-expressions to fold.";
    return r;
}

Optimizer::RuleResult Optimizer::filterSimplification(LogicalPtr& plan) {
    RuleResult r;
    forEachNode(*plan, [&](LogicalNode& n) {
        const auto before = r.details.size();
        forEachExpr(n, [&](BoundExprPtr& e) { simplify(e, r.details); });
        if (r.details.size() > before) r.nodeIds.push_back(n.id);
    });
    prune(plan, ids_, r.details, r.nodeIds);
    r.applied = !r.details.empty();
    r.reason = r.applied ? "Simplified boolean expressions using three-valued-logic identities (x AND TRUE = x, x OR TRUE = TRUE, NOT NOT x = x) and removed filters that are always TRUE or never TRUE."
                         : "No boolean expressions could be simplified.";
    return r;
}

Optimizer::RuleResult Optimizer::predicatePushdown(LogicalPtr& plan) {
    RuleResult r;
    PushdownState st{ids_, r.details, r.nodeIds};
    plan = pushDown(std::move(plan), {}, st);
    r.applied = !r.details.empty();
    r.reason = r.applied ? "Moved filter conditions as close to the data as possible so fewer rows flow through later operators. "
                           "Conditions on the null-extended side of a LEFT JOIN stay above the join to preserve its semantics."
                         : "Filters are already as close to the scans as they can safely go.";
    return r;
}

Optimizer::RuleResult Optimizer::joinReordering(LogicalPtr& plan) {
    RuleResult r;
    bool sawJoinTree = false;
    std::function<void(LogicalPtr&)> visit = [&](LogicalPtr& n) {
        if (!isInnerJoinTree(*n)) {
            for (auto& c : n->children) visit(c);
            return;
        }
        // Count leaves without dismantling first.
        std::function<std::size_t(const LogicalNode&)> countLeaves = [&](const LogicalNode& x) -> std::size_t {
            return isInnerJoinTree(x) ? countLeaves(*x.children[0]) + countLeaves(*x.children[1]) : 1;
        };
        if (countLeaves(*n) < 3) {
            sawJoinTree = true;
            for (auto& c : n->children) visit(c);
            return;
        }
        sawJoinTree = true;
        LogicalPtr original = n->clone();
        cost_.annotate(*original);
        const double originalCost = original->est.totalCost;

        std::vector<LogicalPtr> leaves;
        std::vector<BoundExprPtr> conds;
        collectJoinLeaves(std::move(n), leaves, conds);
        for (auto& leaf : leaves) {
            visit(leaf);  // nested join trees below non-join operators
            cost_.annotate(*leaf);
        }
        std::vector<bool> used(leaves.size(), false), condUsed(conds.size(), false);
        std::vector<std::set<planner::ColumnId>> condCols;
        for (const auto& c : conds) condCols.push_back(columnsOf(*c));

        // Start with the smallest input.
        std::size_t start = 0;
        for (std::size_t i = 1; i < leaves.size(); ++i) {
            if (leaves[i]->est.rows < leaves[start]->est.rows) start = i;
        }
        used[start] = true;
        LogicalPtr tree = std::move(leaves[start]);
        std::string order = leafName(*tree);
        auto available = idsOf(tree->output);

        for (std::size_t step = 1; step < used.size(); ++step) {
            std::size_t best = used.size();
            double bestRows = std::numeric_limits<double>::infinity();
            bool bestConnected = false;
            for (std::size_t i = 0; i < leaves.size(); ++i) {
                if (used[i]) continue;
                auto cols = available;
                for (const auto& c : leaves[i]->output) cols.insert(c.id);
                std::vector<BoundExprPtr> applicable;
                for (std::size_t c = 0; c < conds.size(); ++c) {
                    if (!condUsed[c] && subset(condCols[c], cols)) applicable.push_back(conds[c]->clone());
                }
                const bool connected = !applicable.empty();
                auto combined = planner::combineConjuncts(std::move(applicable));
                const double rows = cost_.joinRows(tree->est.rows, leaves[i]->est.rows, combined.get(), false);
                // Prefer joins that have a condition (avoid cross products).
                if ((connected && !bestConnected) || (connected == bestConnected && rows < bestRows)) {
                    best = i;
                    bestRows = rows;
                    bestConnected = connected;
                }
            }
            used[best] = true;
            for (const auto& c : leaves[best]->output) available.insert(c.id);
            std::vector<BoundExprPtr> applicable;
            for (std::size_t c = 0; c < conds.size(); ++c) {
                if (!condUsed[c] && subset(condCols[c], available)) {
                    applicable.push_back(std::move(conds[c]));
                    condUsed[c] = true;
                }
            }
            order += " ⋈ " + leafName(*leaves[best]);
            auto join = std::make_unique<LogicalNode>();
            join->id = ids_.nextNode++;
            join->op = LogicalOp::Join;
            join->joinType = sql::JoinType::Inner;
            join->predicate = planner::combineConjuncts(std::move(applicable));
            join->output = tree->output;
            join->output.insert(join->output.end(), leaves[best]->output.begin(), leaves[best]->output.end());
            join->children.push_back(std::move(tree));
            join->children.push_back(std::move(leaves[best]));
            cost_.annotate(*join);
            tree = std::move(join);
        }
        // Any condition not yet placed (e.g. referencing no columns) goes on top.
        std::vector<BoundExprPtr> rest;
        for (std::size_t c = 0; c < conds.size(); ++c) {
            if (!condUsed[c]) rest.push_back(std::move(conds[c]));
        }
        if (!rest.empty()) {
            rest.insert(rest.begin(), std::move(tree->predicate));
            tree->predicate = planner::combineConjuncts(std::move(rest));
        }
        cost_.annotate(*tree);
        if (tree->est.totalCost + 1e-9 < originalCost) {
            r.details.push_back("join order " + order + " (estimated cost " + fmtCost(originalCost) + " → " + fmtCost(tree->est.totalCost) + ")");
            r.nodeIds.push_back(tree->id);
            n = std::move(tree);
        } else {
            r.details.push_back("kept the written join order; greedy order " + order + " is not cheaper (" + fmtCost(tree->est.totalCost) +
                                " vs " + fmtCost(originalCost) + ")");
            n = std::move(original);
        }
    };
    visit(plan);
    r.applied = std::any_of(r.details.begin(), r.details.end(), [](const std::string& d) { return d.rfind("join order", 0) == 0; });
    if (r.applied) {
        r.reason = "Reordered inner joins greedily: start from the smallest estimated input and repeatedly add the input that keeps the intermediate result smallest, avoiding cross products.";
    } else if (!r.details.empty()) {
        r.reason = "Considered reordering the inner joins but the estimated cost did not improve.";
    } else {
        bool anyJoin = false;
        forEachNode(*plan, [&](LogicalNode& x) { anyJoin = anyJoin || x.op == LogicalOp::Join; });
        r.reason = sawJoinTree ? "Join reordering needs at least three inner-joined inputs (for two inputs the hash-join build side is chosen during physical planning)."
                   : anyJoin   ? "Only LEFT JOINs are present; their order is fixed by their semantics."
                               : "The query has no joins.";
    }
    return r;
}

Optimizer::RuleResult Optimizer::projectionPushdown(LogicalPtr& plan) {
    RuleResult r;
    prunePlan(*plan, idsOf(plan->output), r.details, r.nodeIds);
    r.applied = !r.details.empty();
    r.reason = r.applied ? "Scans decode only the columns that later operators actually use."
                         : "Every scanned column is used by the query.";
    return r;
}

LogicalPtr Optimizer::optimize(LogicalPtr plan, std::vector<OptimizerStep>& trace) {
    using Rule = RuleResult (Optimizer::*)(LogicalPtr&);
    const std::pair<const char*, Rule> rules[] = {
        {"ConstantFolding", &Optimizer::constantFolding},
        {"FilterSimplification", &Optimizer::filterSimplification},
        {"PredicatePushdown", &Optimizer::predicatePushdown},
        {"JoinReordering", &Optimizer::joinReordering},
        {"ProjectionPushdown", &Optimizer::projectionPushdown},
    };
    for (const auto& [name, rule] : rules) {
        LogicalPtr before = plan->clone();
        cost_.annotate(*before);
        RuleResult res = (this->*rule)(plan);
        cost_.annotate(*plan);
        OptimizerStep step;
        step.rule = name;
        step.phase = "logical";
        step.applied = res.applied;
        step.reason = res.reason;
        step.details = std::move(res.details);
        step.nodeIds = std::move(res.nodeIds);
        step.costBefore = before->est.totalCost;
        step.costAfter = plan->est.totalCost;
        if (res.applied) {
            step.before = std::move(before);
            step.after = plan->clone();
            for (int id : step.nodeIds) {
                forEachNode(*plan, [&](LogicalNode& n) {
                    if (n.id == id) n.notes.push_back(std::string(name) + " changed this operator");
                });
            }
        }
        trace.push_back(std::move(step));
    }
    return plan;
}

}  // namespace querylab::optimizer

#include "querylab/planner/physical_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace querylab::planner {

using optimizer::Alternative;
using optimizer::OptimizerStep;

namespace {

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

std::string exprList(const std::vector<BoundExprPtr>& es) {
    std::string s;
    for (std::size_t i = 0; i < es.size(); ++i) s += (i ? ", " : "") + es[i]->toString();
    return s;
}

}  // namespace

const char* physicalOpName(PhysicalOp op) {
    switch (op) {
        case PhysicalOp::SeqScan: return "Seq Scan";
        case PhysicalOp::IndexScan: return "Index Scan";
        case PhysicalOp::Filter: return "Filter";
        case PhysicalOp::Project: return "Project";
        case PhysicalOp::NestedLoopJoin: return "Nested Loop Join";
        case PhysicalOp::HashJoin: return "Hash Join";
        case PhysicalOp::Sort: return "Sort";
        case PhysicalOp::Limit: return "Limit";
        case PhysicalOp::HashAggregate: return "Hash Aggregate";
        case PhysicalOp::HashDistinct: return "Hash Distinct";
        case PhysicalOp::Values: return "Values";
        case PhysicalOp::Empty: return "Empty Result";
        case PhysicalOp::Update: return "Update";
        case PhysicalOp::Delete: return "Delete";
    }
    return "?";
}

std::string IndexBounds::describe(const catalog::TableInfo& t) const {
    if (!index) return "";
    std::string s;
    std::size_t k = 0;
    for (; k < eqPrefix.size(); ++k) {
        if (!s.empty()) s += " AND ";
        s += t.columns[index->columns[k]].name + " = " + eqPrefix[k].toSqlLiteral();
    }
    if ((lo || hi) && k < index->columns.size()) {
        const std::string& col = t.columns[index->columns[k]].name;
        if (!s.empty()) s += " AND ";
        if (lo && hi) {
            s += lo->toSqlLiteral() + (loInclusive ? " <= " : " < ") + col + (hiInclusive ? " <= " : " < ") + hi->toSqlLiteral();
        } else if (lo) {
            s += col + (loInclusive ? " >= " : " > ") + lo->toSqlLiteral();
        } else {
            s += col + (hiInclusive ? " <= " : " < ") + hi->toSqlLiteral();
        }
    }
    return s.empty() ? "full index scan" : s;
}

std::string PhysicalNode::label() const {
    std::string name = physicalOpName(op);
    switch (op) {
        case PhysicalOp::SeqScan:
            return name + " on " + table->name + (alias != table->name ? " " + alias : "") + (predicate ? " [filter: " + predicate->toString() + "]" : "");
        case PhysicalOp::IndexScan:
            return name + " using " + bounds.index->name + " on " + table->name + (alias != table->name ? " " + alias : "") + " [" +
                   bounds.describe(*table) + "]" + (predicate ? " [filter: " + predicate->toString() + "]" : "");
        case PhysicalOp::Filter: return name + " [" + predicate->toString() + "]";
        case PhysicalOp::Project: return name + " [" + exprList(exprs) + "]";
        case PhysicalOp::NestedLoopJoin:
        case PhysicalOp::HashJoin: {
            std::string s = name + (joinType == sql::JoinType::Left ? " (LEFT)" : "");
            if (op == PhysicalOp::HashJoin) {
                s += " [";
                for (std::size_t i = 0; i < leftKeys.size(); ++i) s += (i ? " AND " : "") + leftKeys[i]->toString() + " = " + rightKeys[i]->toString();
                s += "]";
            }
            if (predicate) s += std::string(op == PhysicalOp::HashJoin ? " [residual: " : " [") + predicate->toString() + "]";
            return s;
        }
        case PhysicalOp::Sort: {
            std::string s = name + " [";
            for (std::size_t i = 0; i < sortKeys.size(); ++i) s += (i ? ", " : "") + sortKeys[i].expr->toString() + (sortKeys[i].descending ? " DESC" : "");
            return s + "]";
        }
        case PhysicalOp::Limit:
            return name + " [" + (limit ? std::to_string(*limit) : "ALL") + (offset ? " OFFSET " + std::to_string(offset) : "") + "]";
        case PhysicalOp::HashAggregate: {
            std::string s = name + " [";
            if (!groupBy.empty()) s += "group by " + exprList(groupBy) + (aggregates.empty() ? "" : "; ");
            for (std::size_t i = 0; i < aggregates.size(); ++i) s += (i ? ", " : "") + aggregates[i].toString();
            return s + "]";
        }
        case PhysicalOp::Update:
        case PhysicalOp::Delete: return name + " on " + table->name;
        default: return name;
    }
}

static void renderPhys(const PhysicalNode& n, const std::string& prefix, bool last, bool root, std::string& out) {
    out += root ? "" : prefix + (last ? "└── " : "├── ");
    out += n.label() + "\n";
    for (std::size_t i = 0; i < n.children.size(); ++i) {
        renderPhys(*n.children[i], root ? "" : prefix + (last ? "    " : "│   "), i + 1 == n.children.size(), false, out);
    }
}

std::string PhysicalNode::toTreeString() const {
    std::string out;
    renderPhys(*this, "", true, true, out);
    return out;
}

Schema tableLayout(const PhysicalNode& scan) {
    Schema layout(scan.table->columns.size());
    for (auto& c : layout) c.id = -1;
    for (std::size_t i = 0; i < scan.scanColumns.size(); ++i) layout[scan.scanColumns[i]] = scan.output[i];
    return layout;
}

// ---- index matching -------------------------------------------------------------

namespace {

struct IndexMatch {
    IndexBounds bounds;
    std::set<std::size_t> used;  // conjunct positions answered by the index
    std::string unusableReason;
};

/// Returns the table position of a column id read by this scan, or -1.
int tablePos(const LogicalNode& scan, ColumnId id) {
    for (std::size_t i = 0; i < scan.output.size(); ++i) {
        if (scan.output[i].id == id) return static_cast<int>(scan.scanColumns[i]);
    }
    return -1;
}

/// Recognizes `column op literal` (or `literal op column`), normalized so
/// the column is on the left.
bool columnVsLiteral(const BoundExpr& e, const LogicalNode& scan, int& pos, std::string& op, Value& lit) {
    if (e.kind != BoundKind::Binary) return false;
    const auto& l = *e.children[0];
    const auto& r = *e.children[1];
    static const std::set<std::string> cmp = {"=", "<", "<=", ">", ">="};
    if (!cmp.count(e.op)) return false;
    if (l.kind == BoundKind::Column && r.kind == BoundKind::Literal) {
        pos = tablePos(scan, l.column);
        op = e.op;
        lit = r.literal;
    } else if (r.kind == BoundKind::Column && l.kind == BoundKind::Literal) {
        pos = tablePos(scan, r.column);
        op = e.op == "<" ? ">" : e.op == "<=" ? ">=" : e.op == ">" ? "<" : e.op == ">=" ? "<=" : e.op;
        lit = l.literal;
    } else {
        return false;
    }
    return pos >= 0 && !lit.isNull();
}

IndexMatch matchIndex(const catalog::IndexInfo& idx, const LogicalNode& scan, const std::vector<BoundExprPtr>& conjuncts) {
    IndexMatch m;
    m.bounds.index = &idx;
    for (std::size_t k = 0; k < idx.columns.size(); ++k) {
        const int want = static_cast<int>(idx.columns[k]);
        bool eqFound = false;
        for (std::size_t c = 0; c < conjuncts.size() && !eqFound; ++c) {
            if (m.used.count(c)) continue;
            int pos;
            std::string op;
            Value lit;
            if (columnVsLiteral(*conjuncts[c], scan, pos, op, lit) && pos == want && op == "=") {
                m.bounds.eqPrefix.push_back(lit);
                m.bounds.usedPredicates.push_back(conjuncts[c]->toString());
                m.used.insert(c);
                eqFound = true;
            }
        }
        if (eqFound) continue;
        // Range on this column, then stop.
        for (std::size_t c = 0; c < conjuncts.size(); ++c) {
            if (m.used.count(c)) continue;
            const BoundExpr& e = *conjuncts[c];
            int pos;
            std::string op;
            Value lit;
            if (columnVsLiteral(e, scan, pos, op, lit) && pos == want) {
                if ((op == ">" || op == ">=") && !m.bounds.lo) {
                    m.bounds.lo = lit;
                    m.bounds.loInclusive = op == ">=";
                } else if ((op == "<" || op == "<=") && !m.bounds.hi) {
                    m.bounds.hi = lit;
                    m.bounds.hiInclusive = op == "<=";
                } else {
                    continue;
                }
                m.bounds.usedPredicates.push_back(e.toString());
                m.used.insert(c);
            } else if (e.kind == BoundKind::Between && !e.negated && e.children[0]->kind == BoundKind::Column &&
                       tablePos(scan, e.children[0]->column) == want && e.children[1]->kind == BoundKind::Literal &&
                       e.children[2]->kind == BoundKind::Literal && !e.children[1]->literal.isNull() &&
                       !e.children[2]->literal.isNull() && !m.bounds.lo && !m.bounds.hi) {
                m.bounds.lo = e.children[1]->literal;
                m.bounds.hi = e.children[2]->literal;
                m.bounds.loInclusive = m.bounds.hiInclusive = true;
                m.bounds.usedPredicates.push_back(e.toString());
                m.used.insert(c);
            }
        }
        break;
    }
    if (m.used.empty()) {
        m.unusableReason = "no equality or range predicate on its leading column '" + scan.table->columns[idx.columns[0]].name + "'";
    }
    return m;
}

}  // namespace

// ---- planning ----------------------------------------------------------------------

PhysicalPtr PhysicalPlanner::plan(const LogicalNode& root) {
    auto p = build(root);
    return p;
}

PhysicalPtr PhysicalPlanner::buildScan(const LogicalNode& n) {
    auto p = std::make_unique<PhysicalNode>();
    p->id = nextId_++;
    p->logicalId = n.id;
    p->table = n.table;
    p->alias = n.alias;
    p->scanColumns = n.scanColumns;
    p->output = n.output;
    p->notes = n.notes;
    p->est.rows = n.est.rows;

    const auto& t = *n.table;
    const double rows = static_cast<double>(t.rowCount());
    const double pages = static_cast<double>(t.pageCount());
    const double colFrac = t.columns.empty() ? 1.0 : static_cast<double>(n.scanColumns.size()) / static_cast<double>(t.columns.size());
    std::vector<BoundExprPtr> conjuncts;
    if (n.predicate) conjuncts = splitConjuncts(n.predicate->clone());

    OptimizerStep step;
    step.rule = "AccessPathSelection";
    step.phase = "physical";
    step.nodeIds = {n.id};
    const double seqCost = cost_.seqScanCost(pages, rows, conjuncts.size(), colFrac);
    step.alternatives.push_back({"Seq Scan on " + t.name, seqCost, false,
                                 "read all " + std::to_string(t.pageCount()) + " pages, " + std::to_string(t.rowCount()) + " rows"});

    double bestCost = seqCost;
    int bestAlt = 0;
    IndexMatch best;
    bool useIndex = false;
    for (const auto* idx : t.indexes) {
        IndexMatch m = matchIndex(*idx, n, conjuncts);
        if (m.used.empty()) {
            step.alternatives.push_back({"Index Scan using " + idx->name, 0, false, "not applicable: " + m.unusableReason});
            continue;
        }
        if (!indexScans_) {
            step.alternatives.push_back({"Index Scan using " + idx->name, 0, false, "disabled for this run"});
            continue;
        }
        std::vector<BoundExprPtr> used;
        for (auto c : m.used) used.push_back(conjuncts[c]->clone());
        auto usedPred = combineConjuncts(std::move(used));
        const double matched = rows * cost_.selectivity(*usedPred);
        const double c = cost_.indexScanCost(static_cast<double>(idx->tree->height()), matched, pages,
                                             conjuncts.size() - m.used.size(), colFrac);
        std::string detail = "index condition " + m.bounds.describe(t) + "; ~" + fmt(matched) + " matching entries (selectivity " +
                             fmt(rows > 0 ? matched / rows * 100 : 0) + "%)";
        step.alternatives.push_back({"Index Scan using " + idx->name, c, false, detail});
        if (c < bestCost) {
            bestCost = c;
            bestAlt = static_cast<int>(step.alternatives.size()) - 1;
            best = std::move(m);
            useIndex = true;
        }
    }
    step.alternatives[static_cast<std::size_t>(bestAlt)].chosen = true;
    step.applied = useIndex;

    std::vector<BoundExprPtr> residual;
    if (useIndex) {
        p->op = PhysicalOp::IndexScan;
        p->bounds = best.bounds;
        for (std::size_t c = 0; c < conjuncts.size(); ++c) {
            if (!best.used.count(c)) residual.push_back(std::move(conjuncts[c]));
        }
        step.reason = "Index Scan using " + best.bounds.index->name + " on " + t.name + " is estimated cheaper (" + fmt(bestCost) +
                      ") than reading the whole table (" + fmt(seqCost) + ").";
        p->notes.push_back("chose " + best.bounds.index->name + ": estimated cost " + fmt(bestCost) + " vs sequential " + fmt(seqCost));
    } else {
        p->op = PhysicalOp::SeqScan;
        residual = std::move(conjuncts);
        if (t.indexes.empty()) {
            step.reason = "Sequential scan of " + t.name + ": the table has no indexes.";
        } else if (step.alternatives.size() > 1 && std::all_of(step.alternatives.begin() + 1, step.alternatives.end(),
                                                               [](const Alternative& a) { return a.cost == 0; })) {
            step.reason = "Sequential scan of " + t.name + ": no index matches the query's predicates.";
        } else {
            step.reason = "Sequential scan of " + t.name + " is estimated cheaper than any index scan — the predicate is not selective enough, "
                          "so fetching rows one by one through an index would touch about as many pages as reading the table.";
        }
        if (!t.indexes.empty()) p->notes.push_back(step.reason);
    }
    p->predicate = combineConjuncts(std::move(residual));
    if (p->predicate) resolveSlots(*p->predicate, tableLayout(*p));
    p->est.selfCost = bestCost;
    trace_.push_back(std::move(step));
    return p;
}

PhysicalPtr PhysicalPlanner::buildJoin(const LogicalNode& n) {
    auto left = build(*n.children[0]);
    auto right = build(*n.children[1]);
    auto p = std::make_unique<PhysicalNode>();
    p->id = nextId_++;
    p->logicalId = n.id;
    p->joinType = n.joinType;
    p->notes = n.notes;
    p->est.rows = n.est.rows;

    const double L = left->est.rows, R = right->est.rows, out = n.est.rows;
    auto keys = optimizer::equiKeys(n.predicate.get(), left->output, right->output);
    const bool inner = n.joinType == sql::JoinType::Inner;

    OptimizerStep step;
    step.rule = "JoinStrategy";
    step.phase = "physical";
    step.nodeIds = {n.id};
    auto sideName = [](const PhysicalNode& x) -> std::string {
        if (x.table) return x.alias;
        return physicalOpName(x.op);
    };
    const double nlCost = cost_.nestedLoopCost(L, R, out);
    step.alternatives.push_back({"Nested Loop Join", nlCost, false, "compare every pair: " + fmt(L) + " × " + fmt(R) + " rows"});
    int choice = 0;  // 0 = NL, 1 = hash build right, 2 = hash build left (swap)
    double bestCost = nlCost;
    if (!keys.empty() && hashJoin_) {
        const double hRight = cost_.hashJoinCost(R, L, out);
        step.alternatives.push_back({"Hash Join (build on " + sideName(*right) + ")", hRight, false,
                                     "build hash table on " + fmt(R) + " rows, probe with " + fmt(L)});
        if (hRight < bestCost) {
            bestCost = hRight;
            choice = 1;
        }
        if (inner) {
            const double hLeft = cost_.hashJoinCost(L, R, out);
            step.alternatives.push_back({"Hash Join (build on " + sideName(*left) + ")", hLeft, false,
                                         "build hash table on " + fmt(L) + " rows, probe with " + fmt(R)});
            if (hLeft < bestCost) {
                bestCost = hLeft;
                choice = 2;
            }
        }
    } else if (keys.empty()) {
        step.alternatives.push_back({"Hash Join", 0, false, "not applicable: the join condition has no column = column equality"});
    } else {
        step.alternatives.push_back({"Hash Join", 0, false, "disabled for this run"});
    }
    const int chosenAlt = choice == 0 ? 0 : (choice == 1 ? 1 : 2);
    step.alternatives[static_cast<std::size_t>(chosenAlt)].chosen = true;
    step.applied = true;
    step.reason = step.alternatives[static_cast<std::size_t>(chosenAlt)].name + " has the lowest estimated cost (" + fmt(bestCost) + ").";
    if (choice == 0 && !keys.empty() && hashJoin_) step.reason += " Inputs are small enough that building a hash table does not pay off.";

    if (choice == 2) {
        std::swap(left, right);
        for (auto& k : keys) std::swap(k.left, k.right);
        p->notes.push_back("inputs swapped so the smaller side (" + sideName(*right) + ") is the hash-table build side");
    }
    p->op = choice == 0 ? PhysicalOp::NestedLoopJoin : PhysicalOp::HashJoin;
    p->output = left->output;
    p->output.insert(p->output.end(), right->output.begin(), right->output.end());

    std::vector<BoundExprPtr> residual;
    if (p->op == PhysicalOp::HashJoin) {
        // Rebuild key expressions and keep non-key conjuncts as a residual.
        std::set<std::pair<ColumnId, ColumnId>> keySet;
        auto findMeta = [](const Schema& s, ColumnId id) {
            for (const auto& c : s) {
                if (c.id == id) return c;
            }
            return ColumnMeta{};
        };
        for (const auto& k : keys) {
            keySet.insert({std::min(k.left, k.right), std::max(k.left, k.right)});
            auto lk = makeColumn(findMeta(left->output, k.left));
            auto rk = makeColumn(findMeta(right->output, k.right));
            resolveSlots(*lk, left->output);
            resolveSlots(*rk, right->output);
            p->leftKeys.push_back(std::move(lk));
            p->rightKeys.push_back(std::move(rk));
        }
        if (n.predicate) {
            for (auto& c : splitConjuncts(n.predicate->clone())) {
                bool isKey = false;
                if (c->kind == BoundKind::Binary && c->op == "=" && c->children[0]->kind == BoundKind::Column &&
                    c->children[1]->kind == BoundKind::Column) {
                    const auto a = c->children[0]->column, b = c->children[1]->column;
                    isKey = keySet.count({std::min(a, b), std::max(a, b)}) > 0;
                }
                if (!isKey) residual.push_back(std::move(c));
            }
        }
    } else if (n.predicate) {
        residual.push_back(n.predicate->clone());
    }
    p->predicate = combineConjuncts(std::move(residual));
    if (p->predicate) resolveSlots(*p->predicate, p->output);
    p->est.selfCost = bestCost;
    p->children.push_back(std::move(left));
    p->children.push_back(std::move(right));
    trace_.push_back(std::move(step));
    return p;
}

PhysicalPtr PhysicalPlanner::build(const LogicalNode& n) {
    if (n.op == LogicalOp::Scan) {
        auto p = buildScan(n);
        finishCost(*p);
        return p;
    }
    if (n.op == LogicalOp::Join) {
        auto p = buildJoin(n);
        finishCost(*p);
        return p;
    }
    auto p = std::make_unique<PhysicalNode>();
    p->id = nextId_++;
    p->logicalId = n.id;
    p->output = n.output;
    p->notes = n.notes;
    p->est.rows = n.est.rows;
    p->est.selfCost = n.est.selfCost;
    for (const auto& c : n.children) p->children.push_back(build(*c));
    const Schema* in = p->children.empty() ? nullptr : &p->children[0]->output;

    switch (n.op) {
        case LogicalOp::Filter:
            p->op = PhysicalOp::Filter;
            p->predicate = n.predicate->clone();
            resolveSlots(*p->predicate, *in);
            break;
        case LogicalOp::Project:
            p->op = PhysicalOp::Project;
            for (const auto& e : n.exprs) {
                p->exprs.push_back(e->clone());
                resolveSlots(*p->exprs.back(), *in);
            }
            break;
        case LogicalOp::Aggregate:
            p->op = PhysicalOp::HashAggregate;
            for (const auto& g : n.groupBy) {
                p->groupBy.push_back(g->clone());
                resolveSlots(*p->groupBy.back(), *in);
            }
            for (const auto& a : n.aggregates) {
                p->aggregates.push_back(a.clone());
                if (p->aggregates.back().arg) resolveSlots(*p->aggregates.back().arg, *in);
            }
            break;
        case LogicalOp::Sort:
            p->op = PhysicalOp::Sort;
            for (const auto& k : n.sortKeys) {
                p->sortKeys.push_back(SortKey{k.expr->clone(), k.descending});
                resolveSlots(*p->sortKeys.back().expr, *in);
            }
            break;
        case LogicalOp::Limit:
            p->op = PhysicalOp::Limit;
            p->limit = n.limit;
            p->offset = n.offset;
            break;
        case LogicalOp::Distinct: p->op = PhysicalOp::HashDistinct; break;
        case LogicalOp::Values: p->op = PhysicalOp::Values; break;
        case LogicalOp::Empty: p->op = PhysicalOp::Empty; break;
        default: throw std::logic_error("unexpected logical operator in physical planning");
    }
    finishCost(*p);
    return p;
}

void PhysicalPlanner::finishCost(PhysicalNode& n) {
    double total = n.est.selfCost;
    for (const auto& c : n.children) total += c->est.totalCost;
    n.est.totalCost = total;
}

PhysicalPtr PhysicalPlanner::planDelete(const LogicalNode& root) {
    auto child = build(root);
    auto p = std::make_unique<PhysicalNode>();
    p->id = nextId_++;
    p->op = PhysicalOp::Delete;
    p->table = root.op == LogicalOp::Scan ? root.table : nullptr;
    const PhysicalNode* scan = child.get();
    while (scan && scan->table == nullptr && !scan->children.empty()) scan = scan->children[0].get();
    if (scan) p->table = scan->table;
    p->est.rows = child->est.rows;
    p->est.selfCost = child->est.rows * cost_.params().cpuTupleCost * 2;
    p->children.push_back(std::move(child));
    finishCost(*p);
    return p;
}

PhysicalPtr PhysicalPlanner::planUpdate(const LogicalNode& root, std::vector<std::pair<std::size_t, BoundExprPtr>> assignments) {
    auto p = planDelete(root);
    p->op = PhysicalOp::Update;
    for (auto& [col, e] : assignments) resolveSlots(*e, p->children[0]->output);
    p->assignments = std::move(assignments);
    return p;
}

}  // namespace querylab::planner

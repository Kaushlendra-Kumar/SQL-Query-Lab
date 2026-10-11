#include "querylab/optimizer/cost_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace querylab::optimizer {

using planner::BoundKind;
using planner::LogicalOp;

namespace {

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

std::string flipOp(const std::string& op) {
    if (op == "<") return ">";
    if (op == "<=") return ">=";
    if (op == ">") return "<";
    if (op == ">=") return "<=";
    return op;
}

bool isComparison(const std::string& op) {
    return op == "=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

void collectConjuncts(const BoundExpr* e, std::vector<const BoundExpr*>& out) {
    if (!e) return;
    if (e->kind == BoundKind::Binary && e->op == "AND") {
        collectConjuncts(e->children[0].get(), out);
        collectConjuncts(e->children[1].get(), out);
    } else {
        out.push_back(e);
    }
}

bool inSchema(ColumnId id, const planner::Schema& s) {
    return std::any_of(s.begin(), s.end(), [&](const ColumnMeta& c) { return c.id == id; });
}

}  // namespace

std::size_t conjunctCount(const BoundExpr* e) {
    std::vector<const BoundExpr*> v;
    collectConjuncts(e, v);
    return v.size();
}

std::vector<EquiKey> equiKeys(const BoundExpr* cond, const planner::Schema& l, const planner::Schema& r) {
    std::vector<const BoundExpr*> terms;
    collectConjuncts(cond, terms);
    std::vector<EquiKey> keys;
    for (const auto* t : terms) {
        if (t->kind != BoundKind::Binary || t->op != "=") continue;
        const auto& a = *t->children[0];
        const auto& b = *t->children[1];
        if (a.kind != BoundKind::Column || b.kind != BoundKind::Column) continue;
        if (inSchema(a.column, l) && inSchema(b.column, r)) keys.push_back({a.column, b.column});
        else if (inSchema(b.column, l) && inSchema(a.column, r)) keys.push_back({b.column, a.column});
    }
    return keys;
}

void CostModel::indexColumns(const LogicalNode& root) {
    for (const auto& c : root.output) columns_[c.id] = c;
    for (const auto& a : root.aggregates) columns_[a.output.id] = a.output;
    for (const auto& ch : root.children) indexColumns(*ch);
}

const ColumnMeta* CostModel::column(ColumnId id) const {
    auto it = columns_.find(id);
    return it == columns_.end() ? nullptr : &it->second;
}

const catalog::ColumnStats* CostModel::statsFor(ColumnId c, double* tableRows) const {
    const ColumnMeta* m = column(c);
    if (!m || !m->sourceTable || m->sourceColumn < 0) return nullptr;
    if (tableRows) *tableRows = static_cast<double>(m->sourceTable->rowCount());
    return &m->sourceTable->stats[static_cast<std::size_t>(m->sourceColumn)];
}

double CostModel::distinctValues(ColumnId c, double fallbackRows) const {
    double rows = 0;
    const auto* st = statsFor(c, &rows);
    if (!st) return std::max(1.0, fallbackRows);
    return std::max<double>(1.0, static_cast<double>(st->distinct()) + (st->nullCount() ? 1.0 : 0.0));
}

double CostModel::rangeSelectivity(ColumnId col, const Value* lo, bool loInc, const Value* hi, bool hiInc) const {
    double rows = 0;
    const auto* st = statsFor(col, &rows);
    if (!st || rows <= 0) return p_.defaultSelectivity;
    const double nonNullFrac = static_cast<double>(st->nonNullCount()) / rows;
    const double below = hi ? st->fractionBelow(*hi, hiInc) : 1.0;
    const double atOrBelowLo = lo ? st->fractionBelow(*lo, !loInc) : 0.0;
    return std::clamp(below - atOrBelowLo, 0.0, 1.0) * nonNullFrac;
}

double CostModel::comparisonSelectivity(const std::string& op, ColumnId col, const Value& v) const {
    if (v.isNull()) return 0.0;  // comparison with NULL is never TRUE
    double rows = 0;
    const auto* st = statsFor(col, &rows);
    if (!st || rows <= 0) return op == "=" ? 0.005 : (op == "<>" ? 0.995 : p_.defaultSelectivity);
    const double nonNull = static_cast<double>(st->nonNullCount());
    const double nonNullFrac = nonNull / rows;
    const double ndv = std::max<double>(1.0, static_cast<double>(st->distinct()));
    if (op == "=" || op == "<>") {
        double eq = 1.0 / ndv;
        auto mn = st->min();
        auto mx = st->max();
        if (mn && mx && (Value::compareTotal(v, *mn) < 0 || Value::compareTotal(v, *mx) > 0)) eq = 0.0;
        return (op == "=" ? eq : 1.0 - eq) * nonNullFrac;
    }
    if (op == "<") return rangeSelectivity(col, nullptr, false, &v, false);
    if (op == "<=") return rangeSelectivity(col, nullptr, false, &v, true);
    if (op == ">") return rangeSelectivity(col, &v, false, nullptr, false);
    return rangeSelectivity(col, &v, true, nullptr, false);
}

double CostModel::selectivity(const BoundExpr& e) const {
    double s = p_.defaultSelectivity;
    switch (e.kind) {
        case BoundKind::Literal:
            s = (e.literal.type() == DataType::Boolean && e.literal.asBool()) ? 1.0 : 0.0;
            break;
        case BoundKind::Unary:
            s = e.op == "NOT" ? 1.0 - selectivity(*e.children[0]) : p_.defaultSelectivity;
            break;
        case BoundKind::Binary: {
            if (e.op == "AND") {
                s = selectivity(*e.children[0]) * selectivity(*e.children[1]);  // independence assumption
                break;
            }
            if (e.op == "OR") {
                const double a = selectivity(*e.children[0]), b = selectivity(*e.children[1]);
                s = a + b - a * b;
                break;
            }
            if (!isComparison(e.op)) break;
            const auto& l = *e.children[0];
            const auto& r = *e.children[1];
            if (l.kind == BoundKind::Column && r.isConstant() && r.kind == BoundKind::Literal) {
                s = comparisonSelectivity(e.op, l.column, r.literal);
            } else if (r.kind == BoundKind::Column && l.kind == BoundKind::Literal) {
                s = comparisonSelectivity(flipOp(e.op), r.column, l.literal);
            } else if (l.kind == BoundKind::Column && r.kind == BoundKind::Column && e.op == "=") {
                s = 1.0 / std::max(distinctValues(l.column, 10), distinctValues(r.column, 10));
            } else if (e.op == "=") {
                s = 0.005;
            }
            break;
        }
        case BoundKind::IsNull: {
            const auto& c = *e.children[0];
            double rows = 0;
            const auto* st = c.kind == BoundKind::Column ? statsFor(c.column, &rows) : nullptr;
            const double nullFrac = (st && rows > 0) ? static_cast<double>(st->nullCount()) / rows : 0.01;
            s = e.negated ? 1.0 - nullFrac : nullFrac;
            break;
        }
        case BoundKind::Between: {
            const auto& c = *e.children[0];
            if (c.kind == BoundKind::Column && e.children[1]->kind == BoundKind::Literal && e.children[2]->kind == BoundKind::Literal) {
                s = rangeSelectivity(c.column, &e.children[1]->literal, true, &e.children[2]->literal, true);
            } else {
                s = 0.25;
            }
            if (e.negated) s = 1.0 - s;
            break;
        }
        case BoundKind::InList: {
            const auto& c = *e.children[0];
            double total = 0;
            for (std::size_t i = 1; i < e.children.size(); ++i) {
                if (c.kind == BoundKind::Column && e.children[i]->kind == BoundKind::Literal) {
                    total += comparisonSelectivity("=", c.column, e.children[i]->literal);
                } else {
                    total += 0.005;
                }
            }
            s = std::min(1.0, total);
            if (e.negated) s = 1.0 - s;
            break;
        }
        case BoundKind::Like: {
            const auto& pat = *e.children[1];
            s = 0.1;
            if (pat.kind == BoundKind::Literal && pat.literal.type() == DataType::Text) {
                const std::string& p = pat.literal.asText();
                const auto wc = p.find_first_of("%_");
                if (wc == std::string::npos && e.children[0]->kind == BoundKind::Column) {
                    s = comparisonSelectivity("=", e.children[0]->column, pat.literal);
                } else if (wc != std::string::npos && wc > 0) {
                    s = 0.05;  // fixed prefix
                }
            }
            if (e.negated) s = 1.0 - s;
            break;
        }
        default:
            break;
    }
    return std::clamp(s, 0.0, 1.0);
}

std::string CostModel::explainSelectivity(const BoundExpr& e) const {
    std::vector<const BoundExpr*> terms;
    collectConjuncts(&e, terms);
    std::string out;
    for (const auto* t : terms) {
        if (!out.empty()) out += "; ";
        std::string how = "default guess";
        if (t->kind == BoundKind::Binary && isComparison(t->op)) {
            const bool colConst = (t->children[0]->kind == BoundKind::Column && t->children[1]->kind == BoundKind::Literal) ||
                                  (t->children[1]->kind == BoundKind::Column && t->children[0]->kind == BoundKind::Literal);
            if (colConst) how = (t->op == "=" || t->op == "<>") ? "1/distinct values (uniformity assumption)" : "histogram range estimate";
            else if (t->children[0]->kind == BoundKind::Column && t->children[1]->kind == BoundKind::Column) how = "1/max(distinct values)";
        } else if (t->kind == BoundKind::Between) {
            how = "histogram range estimate";
        } else if (t->kind == BoundKind::IsNull) {
            how = "null fraction";
        } else if (t->kind == BoundKind::InList) {
            how = "sum of equality estimates";
        } else if (t->kind == BoundKind::Like) {
            how = "fixed LIKE heuristic";
        } else if (t->kind == BoundKind::Binary && t->op == "OR") {
            how = "s1 + s2 - s1*s2";
        }
        out += t->toString() + " → " + fmt(selectivity(*t)) + " (" + how + ")";
    }
    if (terms.size() > 1) out += "; combined assuming independence → " + fmt(selectivity(e));
    return out;
}

double CostModel::joinRows(double left, double right, const BoundExpr* cond, bool leftOuter) const {
    double rows = left * right;
    if (cond) rows *= selectivity(*cond);
    if (leftOuter) rows = std::max(rows, left);
    return std::max(rows, 0.0);
}

double CostModel::seqScanCost(double pages, double rows, std::size_t terms, double colFrac) const {
    // Decoding cost shrinks when fewer columns are read (projection pushdown).
    return pages * p_.seqPageCost + rows * p_.cpuTupleCost * (0.5 + 0.5 * colFrac) +
           rows * p_.cpuOperatorCost * static_cast<double>(terms);
}

double CostModel::indexScanCost(double height, double matched, double heapPages, std::size_t residual, double colFrac) const {
    // Descend the tree, walk matching leaf entries, then fetch each row.
    // Distinct heap pages touched ≈ min(matched, pages): rows with nearby
    // keys are usually not on the same page, so this is pessimistic.
    const double leafNodes = std::ceil(matched / 32.0);
    const double distinctPages = std::min(matched, heapPages);
    return (height + leafNodes) * p_.indexNodeCost + matched * p_.indexEntryCost + distinctPages * p_.randomPageCost +
           matched * (p_.bufferLookupCost + p_.cpuTupleCost * (0.5 + 0.5 * colFrac)) + matched * p_.cpuOperatorCost * static_cast<double>(residual);
}

double CostModel::hashJoinCost(double buildRows, double probeRows, double outRows) const {
    return buildRows * p_.hashBuildCost + probeRows * p_.hashProbeCost + outRows * p_.cpuTupleCost;
}

double CostModel::nestedLoopCost(double outerRows, double innerRows, double outRows) const {
    return outerRows * innerRows * p_.cpuOperatorCost + outRows * p_.cpuTupleCost;
}

double CostModel::sortCost(double rows, std::size_t keys) const {
    if (rows < 2) return rows * p_.cpuTupleCost;
    return rows * std::log2(rows) * p_.sortCompareCost * static_cast<double>(std::max<std::size_t>(keys, 1)) + rows * p_.cpuTupleCost;
}

double CostModel::hashAggCost(double rows, std::size_t groups, std::size_t aggs) const {
    return rows * (p_.hashProbeCost + p_.cpuOperatorCost * static_cast<double>(aggs + groups));
}

void CostModel::annotate(LogicalNode& n) const {
    for (auto& c : n.children) annotate(*c);
    auto childRows = [&](std::size_t i) { return n.children[i]->est.rows; };
    double self = 0;
    double rows = 0;
    switch (n.op) {
        case LogicalOp::Scan: {
            const double base = static_cast<double>(n.table->rowCount());
            rows = base * (n.predicate ? selectivity(*n.predicate) : 1.0);
            const double colFrac = n.table->columns.empty() ? 1.0
                                                            : static_cast<double>(n.scanColumns.size()) / static_cast<double>(n.table->columns.size());
            self = seqScanCost(static_cast<double>(n.table->pageCount()), base, conjunctCount(n.predicate.get()), colFrac);
            break;
        }
        case LogicalOp::Filter:
            rows = childRows(0) * selectivity(*n.predicate);
            self = childRows(0) * p_.cpuOperatorCost * static_cast<double>(conjunctCount(n.predicate.get()));
            break;
        case LogicalOp::Project:
            rows = childRows(0);
            self = rows * p_.cpuOperatorCost * static_cast<double>(n.exprs.size()) * 0.1;
            break;
        case LogicalOp::Join: {
            const double l = childRows(0), r = childRows(1);
            rows = joinRows(l, r, n.predicate.get(), n.joinType == sql::JoinType::Left);
            const bool equi = !equiKeys(n.predicate.get(), n.children[0]->output, n.children[1]->output).empty();
            self = equi ? hashJoinCost(r, l, rows) : nestedLoopCost(l, r, rows);
            break;
        }
        case LogicalOp::Aggregate: {
            const double in = childRows(0);
            if (n.groupBy.empty()) {
                rows = 1;
            } else {
                double g = 1;
                for (const auto& e : n.groupBy) {
                    g *= e->kind == BoundKind::Column ? distinctValues(e->column, in) : std::max(1.0, in / 10.0);
                }
                rows = std::max(1.0, std::min(g, in));
            }
            self = hashAggCost(in, n.groupBy.size(), n.aggregates.size());
            break;
        }
        case LogicalOp::Sort:
            rows = childRows(0);
            self = sortCost(rows, n.sortKeys.size());
            break;
        case LogicalOp::Limit:
            rows = std::max(0.0, childRows(0) - static_cast<double>(n.offset));
            if (n.limit) rows = std::min(rows, static_cast<double>(*n.limit));
            break;
        case LogicalOp::Distinct: {
            const double in = childRows(0);
            double d = 1;
            bool known = true;
            for (const auto& c : n.output) {
                if (!c.sourceTable) known = false;
                else d *= distinctValues(c.id, in);
            }
            rows = known ? std::max(1.0, std::min(in, d)) : in;
            self = in * p_.hashBuildCost;
            break;
        }
        case LogicalOp::Values: rows = 1; break;
        case LogicalOp::Empty: rows = 0; break;
    }
    double total = self;
    for (const auto& c : n.children) total += c->est.totalCost;
    n.est.rows = rows;
    n.est.selfCost = self;
    n.est.totalCost = total;
}

}  // namespace querylab::optimizer

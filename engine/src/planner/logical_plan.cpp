#include "querylab/planner/logical_plan.h"

#include <cctype>

namespace querylab::planner {

const char* logicalOpName(LogicalOp op) {
    switch (op) {
        case LogicalOp::Scan: return "Scan";
        case LogicalOp::Filter: return "Filter";
        case LogicalOp::Project: return "Project";
        case LogicalOp::Join: return "Join";
        case LogicalOp::Aggregate: return "Aggregate";
        case LogicalOp::Sort: return "Sort";
        case LogicalOp::Limit: return "Limit";
        case LogicalOp::Distinct: return "Distinct";
        case LogicalOp::Values: return "Values";
        case LogicalOp::Empty: return "Empty";
    }
    return "?";
}

std::string AggregateCall::toString() const {
    std::string f;
    for (char c : func) f += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!arg) return f + "(*)";
    return f + "(" + (distinct ? "DISTINCT " : "") + arg->toString() + ")";
}

AggregateCall AggregateCall::clone() const {
    AggregateCall a;
    a.func = func;
    a.arg = arg ? arg->clone() : nullptr;
    a.distinct = distinct;
    a.output = output;
    return a;
}

std::unique_ptr<LogicalNode> LogicalNode::clone() const {
    auto n = std::make_unique<LogicalNode>();
    n->id = id;
    n->op = op;
    for (const auto& c : children) n->children.push_back(c->clone());
    n->output = output;
    n->table = table;
    n->alias = alias;
    n->scanColumns = scanColumns;
    n->predicate = predicate ? predicate->clone() : nullptr;
    n->joinType = joinType;
    for (const auto& e : exprs) n->exprs.push_back(e->clone());
    for (const auto& e : groupBy) n->groupBy.push_back(e->clone());
    for (const auto& a : aggregates) n->aggregates.push_back(a.clone());
    for (const auto& k : sortKeys) n->sortKeys.push_back(SortKey{k.expr->clone(), k.descending});
    n->limit = limit;
    n->offset = offset;
    n->est = est;
    n->notes = notes;
    return n;
}

std::string LogicalNode::label() const {
    auto list = [](const std::vector<BoundExprPtr>& es) {
        std::string s;
        for (std::size_t i = 0; i < es.size(); ++i) s += (i ? ", " : "") + es[i]->toString();
        return s;
    };
    switch (op) {
        case LogicalOp::Scan: {
            std::string s = "Scan(" + table->name;
            if (alias != table->name) s += " AS " + alias;
            if (predicate) s += " WHERE " + predicate->toString();
            return s + ")";
        }
        case LogicalOp::Filter: return "Filter(" + predicate->toString() + ")";
        case LogicalOp::Project: {
            std::string s = "Project(";
            for (std::size_t i = 0; i < exprs.size(); ++i) {
                s += (i ? ", " : "") + exprs[i]->toString();
                const bool sameName = exprs[i]->kind == BoundKind::Column ? exprs[i]->columnName.ends_with(output[i].name)
                                                                         : exprs[i]->toString() == output[i].name;
                if (!sameName) s += " AS " + output[i].name;
            }
            return s + ")";
        }
        case LogicalOp::Join: {
            std::string kind = joinType == sql::JoinType::Left ? "LeftJoin" : (predicate ? "InnerJoin" : "CrossJoin");
            return kind + (predicate ? "(" + predicate->toString() + ")" : "");
        }
        case LogicalOp::Aggregate: {
            std::string s = "Aggregate(";
            if (!groupBy.empty()) s += "group by " + list(groupBy) + (aggregates.empty() ? "" : "; ");
            for (std::size_t i = 0; i < aggregates.size(); ++i) s += (i ? ", " : "") + aggregates[i].toString();
            return s + ")";
        }
        case LogicalOp::Sort: {
            std::string s = "Sort(";
            for (std::size_t i = 0; i < sortKeys.size(); ++i) {
                s += (i ? ", " : "") + sortKeys[i].expr->toString() + (sortKeys[i].descending ? " DESC" : "");
            }
            return s + ")";
        }
        case LogicalOp::Limit: {
            std::string s = "Limit(";
            s += limit ? std::to_string(*limit) : "ALL";
            if (offset) s += " OFFSET " + std::to_string(offset);
            return s + ")";
        }
        case LogicalOp::Distinct: return "Distinct";
        case LogicalOp::Values: return "Values(1 row)";
        case LogicalOp::Empty: return "Empty";
    }
    return "?";
}

static void render(const LogicalNode& n, const std::string& prefix, bool last, bool root, std::string& out) {
    out += root ? "" : prefix + (last ? "└── " : "├── ");
    out += n.label() + "\n";
    for (std::size_t i = 0; i < n.children.size(); ++i) {
        render(*n.children[i], root ? "" : prefix + (last ? "    " : "│   "), i + 1 == n.children.size(), false, out);
    }
}

std::string LogicalNode::toTreeString() const {
    std::string out;
    render(*this, "", true, true, out);
    return out;
}

}  // namespace querylab::planner

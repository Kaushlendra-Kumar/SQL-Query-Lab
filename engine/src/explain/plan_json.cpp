#include "querylab/explain/plan_json.h"

namespace querylab::explain {

using planner::LogicalOp;
using planner::PhysicalOp;

const char* describeLogicalOp(LogicalOp op) {
    switch (op) {
        case LogicalOp::Scan: return "Produces the rows of a base table.";
        case LogicalOp::Filter: return "Keeps only rows for which the predicate is TRUE (FALSE and NULL are discarded).";
        case LogicalOp::Project: return "Computes the output columns from each input row.";
        case LogicalOp::Join: return "Combines rows from two inputs that satisfy the join condition.";
        case LogicalOp::Aggregate: return "Groups rows and computes aggregate functions per group.";
        case LogicalOp::Sort: return "Orders rows by the sort keys.";
        case LogicalOp::Limit: return "Skips OFFSET rows and returns at most LIMIT rows.";
        case LogicalOp::Distinct: return "Removes duplicate rows.";
        case LogicalOp::Values: return "Produces a single empty row (SELECT without FROM).";
        case LogicalOp::Empty: return "Produces no rows (the optimizer proved the result is empty).";
    }
    return "";
}

const char* describePhysicalOp(PhysicalOp op) {
    switch (op) {
        case PhysicalOp::SeqScan:
            return "Reads every page of the table in order and evaluates the pushed-down filter on each row. Columns used by the filter "
                   "are decoded first; the remaining columns are decoded only for rows that pass.";
        case PhysicalOp::IndexScan:
            return "Walks the B+ tree from the first matching key, then fetches each matching row from its heap page by row id. "
                   "Fast when few rows match; each match is a separate page access.";
        case PhysicalOp::Filter: return "Evaluates the predicate on each input row and passes on rows where it is TRUE.";
        case PhysicalOp::Project: return "Evaluates the output expressions for each row.";
        case PhysicalOp::NestedLoopJoin:
            return "Buffers the right input, then compares every left row with every buffered right row. Works for any join "
                   "condition but costs |left| × |right| comparisons.";
        case PhysicalOp::HashJoin:
            return "Builds a hash table on the right (build) input keyed by the join columns, then streams the left (probe) input and "
                   "looks up matches. Needs an equality condition; roughly linear in the input sizes.";
        case PhysicalOp::Sort: return "Buffers all input rows and sorts them (stable sort, NULLs first in ascending order).";
        case PhysicalOp::Limit: return "Skips OFFSET rows and stops pulling input once LIMIT rows have been returned.";
        case PhysicalOp::HashAggregate: return "Buffers one entry per group in a hash table, updating aggregates as rows arrive.";
        case PhysicalOp::HashDistinct: return "Remembers every row seen in a hash set and emits only the first occurrence.";
        case PhysicalOp::Values: return "Produces a single empty row.";
        case PhysicalOp::Empty: return "Produces no rows without reading any data.";
        case PhysicalOp::Update: return "Collects the target rows first, then updates them (and every index on the table).";
        case PhysicalOp::Delete: return "Collects the target rows first, then deletes them (and their index entries).";
    }
    return "";
}

json schemaToJson(const planner::Schema& s) {
    json arr = json::array();
    for (const auto& c : s) {
        json col{{"id", c.id}, {"name", c.name}, {"type", typeName(c.type)}};
        if (!c.qualifier.empty()) col["qualifier"] = c.qualifier;
        if (c.sourceTable) col["source"] = c.sourceTable->name + "." + c.sourceTable->columns[static_cast<std::size_t>(c.sourceColumn)].name;
        arr.push_back(std::move(col));
    }
    return arr;
}

static json exprs(const std::vector<planner::BoundExprPtr>& es) {
    json a = json::array();
    for (const auto& e : es) a.push_back(e->toString());
    return a;
}

json logicalToJson(const planner::LogicalNode& n) {
    json details = json::object();
    switch (n.op) {
        case LogicalOp::Scan: {
            details["table"] = n.table->name;
            details["alias"] = n.alias;
            json cols = json::array();
            for (auto c : n.scanColumns) cols.push_back(n.table->columns[c].name);
            details["columnsRead"] = cols;
            details["tableColumns"] = n.table->columns.size();
            details["tableRows"] = n.table->rowCount();
            if (n.predicate) details["pushedPredicate"] = n.predicate->toString();
            break;
        }
        case LogicalOp::Filter: details["predicate"] = n.predicate->toString(); break;
        case LogicalOp::Project: details["expressions"] = exprs(n.exprs); break;
        case LogicalOp::Join:
            details["joinType"] = n.joinType == sql::JoinType::Left ? "LEFT" : (n.predicate ? "INNER" : "CROSS");
            if (n.predicate) details["condition"] = n.predicate->toString();
            break;
        case LogicalOp::Aggregate: {
            details["groupBy"] = exprs(n.groupBy);
            json a = json::array();
            for (const auto& x : n.aggregates) a.push_back(x.toString());
            details["aggregates"] = a;
            break;
        }
        case LogicalOp::Sort: {
            json k = json::array();
            for (const auto& s : n.sortKeys) k.push_back(s.expr->toString() + (s.descending ? " DESC" : " ASC"));
            details["keys"] = k;
            break;
        }
        case LogicalOp::Limit:
            details["limit"] = n.limit ? json(*n.limit) : json(nullptr);
            details["offset"] = n.offset;
            break;
        default: break;
    }
    json children = json::array();
    for (const auto& c : n.children) children.push_back(logicalToJson(*c));
    return json{{"id", n.id},
                {"kind", "logical"},
                {"op", planner::logicalOpName(n.op)},
                {"label", n.label()},
                {"description", describeLogicalOp(n.op)},
                {"details", details},
                {"output", schemaToJson(n.output)},
                {"estimates", {{"rows", n.est.rows}, {"cost", n.est.totalCost}, {"selfCost", n.est.selfCost}}},
                {"notes", n.notes},
                {"children", children}};
}

json physicalToJson(const planner::PhysicalNode& n, const std::map<int, execution::OperatorStats>* stats,
                    const optimizer::CostModel& cost) {
    json details = json::object();
    if (n.table) {
        details["table"] = n.table->name;
        details["alias"] = n.alias;
        details["tableRows"] = n.table->rowCount();
        details["tablePages"] = n.table->pageCount();
    }
    switch (n.op) {
        case PhysicalOp::SeqScan:
        case PhysicalOp::IndexScan: {
            json cols = json::array();
            for (auto c : n.scanColumns) cols.push_back(n.table->columns[c].name);
            details["columnsRead"] = cols;
            details["tableColumns"] = n.table->columns.size();
            if (n.predicate) {
                details["filter"] = n.predicate->toString();
                details["selectivityEstimate"] = cost.explainSelectivity(*n.predicate);
            }
            if (n.op == PhysicalOp::IndexScan) {
                const auto& idx = *n.bounds.index;
                details["index"] = idx.name;
                details["indexDefinition"] = idx.definition(*n.table);
                details["indexCondition"] = n.bounds.describe(*n.table);
                details["indexPredicates"] = n.bounds.usedPredicates;
                details["indexHeight"] = idx.tree->height();
                details["indexEntries"] = idx.tree->size();
            }
            break;
        }
        case PhysicalOp::Filter: details["predicate"] = n.predicate->toString(); break;
        case PhysicalOp::Project: details["expressions"] = exprs(n.exprs); break;
        case PhysicalOp::HashJoin:
        case PhysicalOp::NestedLoopJoin: {
            details["joinType"] = n.joinType == sql::JoinType::Left ? "LEFT" : "INNER";
            if (n.op == PhysicalOp::HashJoin) {
                json keys = json::array();
                for (std::size_t i = 0; i < n.leftKeys.size(); ++i) keys.push_back(n.leftKeys[i]->toString() + " = " + n.rightKeys[i]->toString());
                details["hashKeys"] = keys;
                details["buildSide"] = "right input";
                details["probeSide"] = "left input";
            }
            if (n.predicate) details[n.op == PhysicalOp::HashJoin ? "residual" : "condition"] = n.predicate->toString();
            break;
        }
        case PhysicalOp::HashAggregate: {
            details["groupBy"] = exprs(n.groupBy);
            json a = json::array();
            for (const auto& x : n.aggregates) a.push_back(x.toString());
            details["aggregates"] = a;
            break;
        }
        case PhysicalOp::Sort: {
            json k = json::array();
            for (const auto& s : n.sortKeys) k.push_back(s.expr->toString() + (s.descending ? " DESC" : " ASC"));
            details["keys"] = k;
            break;
        }
        case PhysicalOp::Limit:
            details["limit"] = n.limit ? json(*n.limit) : json(nullptr);
            details["offset"] = n.offset;
            break;
        case PhysicalOp::Update: {
            json a = json::array();
            for (const auto& [col, e] : n.assignments) a.push_back(n.table->columns[col].name + " = " + e->toString());
            details["assignments"] = a;
            break;
        }
        default: break;
    }

    json children = json::array();
    std::int64_t childNs = 0;
    for (const auto& c : n.children) {
        children.push_back(physicalToJson(*c, stats, cost));
        if (stats) {
            auto it = stats->find(c->id);
            if (it != stats->end()) childNs += it->second.inclusiveNs;
        }
    }

    json out{{"id", n.id},
             {"logicalId", n.logicalId},
             {"kind", "physical"},
             {"op", planner::physicalOpName(n.op)},
             {"label", n.label()},
             {"description", describePhysicalOp(n.op)},
             {"details", details},
             {"output", schemaToJson(n.output)},
             {"estimates", {{"rows", n.est.rows}, {"cost", n.est.totalCost}, {"selfCost", n.est.selfCost}}},
             {"notes", n.notes},
             {"children", children}};

    if (stats) {
        auto it = stats->find(n.id);
        if (it != stats->end()) {
            const auto& s = it->second;
            out["actual"] = {{"executed", s.opened},
                             {"completed", s.exhausted},
                             {"rowsIn", s.rowsIn},
                             {"rowsOut", s.rowsOut},
                             {"rowsFiltered", s.rowsFiltered},
                             {"rowsScanned", s.rowsScanned},
                             {"pagesRead", s.pagesRead},
                             {"pageMisses", s.pageMisses},
                             {"indexNodesVisited", s.indexNodesVisited},
                             {"indexEntriesExamined", s.indexEntriesExamined},
                             {"nextCalls", s.nextCalls},
                             {"buildRows", s.buildRows},
                             {"groups", s.groups},
                             {"timeMs", static_cast<double>(s.inclusiveNs) / 1e6},
                             {"selfTimeMs", static_cast<double>(std::max<std::int64_t>(0, s.inclusiveNs - childNs)) / 1e6},
                             {"peakMemoryBytes", s.peakMemoryBytes}};
        } else {
            out["actual"] = {{"executed", false}};
        }
    }
    return out;
}

json optimizerTraceToJson(const std::vector<optimizer::OptimizerStep>& steps) {
    json arr = json::array();
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& s = steps[i];
        json alts = json::array();
        for (const auto& a : s.alternatives) {
            alts.push_back({{"name", a.name}, {"cost", a.cost}, {"chosen", a.chosen}, {"detail", a.detail}, {"applicable", a.cost > 0 || a.chosen}});
        }
        json step{{"index", i},
                  {"rule", s.rule},
                  {"phase", s.phase},
                  {"applied", s.applied},
                  {"reason", s.reason},
                  {"details", s.details},
                  {"costBefore", s.costBefore},
                  {"costAfter", s.costAfter},
                  {"alternatives", alts},
                  {"nodeIds", s.nodeIds}};
        if (s.before) step["before"] = logicalToJson(*s.before);
        if (s.after) step["after"] = logicalToJson(*s.after);
        arr.push_back(std::move(step));
    }
    return arr;
}

}  // namespace querylab::explain

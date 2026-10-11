#include "querylab/explain/insights.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>

namespace querylab::explain {

using planner::BoundExpr;
using planner::BoundKind;
using planner::PhysicalNode;
using planner::PhysicalOp;

namespace {

std::string num(double v, int decimals = 0) {
    char buf[48];
    if (decimals == 0) {
        // thousands separators for readability
        std::snprintf(buf, sizeof buf, "%.0f", v);
        std::string s(buf), out;
        const bool neg = !s.empty() && s[0] == '-';
        if (neg) s = s.substr(1);
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (i && (s.size() - i) % 3 == 0) out += ',';
            out += s[i];
        }
        return (neg ? "-" : "") + out;
    }
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string pct(double part, double whole) {
    if (whole <= 0) return "0%";
    const double p = part / whole * 100.0;
    return num(p, p < 1 && p > 0 ? 2 : 1) + "%";
}

void postOrder(const PhysicalNode& n, const std::function<void(const PhysicalNode&)>& fn) {
    for (const auto& c : n.children) postOrder(*c, fn);
    fn(n);
}

const execution::OperatorStats* statOf(const InsightInput& in, int id) {
    if (!in.stats) return nullptr;
    auto it = in.stats->find(id);
    return it == in.stats->end() ? nullptr : &it->second;
}

std::string opShort(const PhysicalNode& n) {
    std::string s = planner::physicalOpName(n.op);
    if (n.table) s += " (" + n.alias + ")";
    return s;
}

void conjuncts(const BoundExpr* e, std::vector<const BoundExpr*>& out) {
    if (!e) return;
    if (e->kind == BoundKind::Binary && e->op == "AND") {
        conjuncts(e->children[0].get(), out);
        conjuncts(e->children[1].get(), out);
    } else {
        out.push_back(e);
    }
}

std::string describeIntent(const sql::Statement& st) {
    if (!st.isSelect()) return std::string("Run a ") + st.kindName() + " statement.";
    const auto& s = std::get<sql::SelectStmt>(st.body);
    std::string cols;
    for (std::size_t i = 0; i < s.items.size(); ++i) {
        cols += (i ? ", " : "") + s.items[i].expr->toString();
        if (!s.items[i].alias.empty()) cols += " AS " + s.items[i].alias;
    }
    std::string out = s.distinct ? "Return the distinct values of " + cols : "Return " + cols;
    if (s.from) {
        out += " from " + s.from->name;
        for (const auto& j : s.joins) {
            out += std::string(j.type == sql::JoinType::Left ? ", left-joined with " : ", joined with ") + j.table.name;
            if (j.on) out += " on " + j.on->toString();
        }
    }
    if (s.where) out += ", keeping only rows where " + s.where->toString();
    if (!s.groupBy.empty()) {
        out += ", grouped by ";
        for (std::size_t i = 0; i < s.groupBy.size(); ++i) out += (i ? ", " : "") + s.groupBy[i]->toString();
    }
    if (s.having) out += ", keeping groups where " + s.having->toString();
    if (!s.orderBy.empty()) {
        out += ", sorted by ";
        for (std::size_t i = 0; i < s.orderBy.size(); ++i) {
            out += (i ? ", " : "") + s.orderBy[i].expr->toString() + (s.orderBy[i].descending ? " (descending)" : "");
        }
    }
    if (s.limit) out += ", returning at most " + std::to_string(*s.limit) + " rows";
    if (s.offset) out += " after skipping " + std::to_string(*s.offset);
    return out + ".";
}

}  // namespace

json explainExecution(const InsightInput& in) {
    json facts = json::array();
    json guidance = json::array();
    json warnings = json::array();
    auto fact = [&](const std::string& topic, const std::string& text, int nodeId = 0) {
        json f{{"topic", topic}, {"text", text}};
        if (nodeId) f["nodeId"] = nodeId;
        facts.push_back(std::move(f));
    };
    auto guide = [&](const std::string& text) { guidance.push_back(text); };

    const auto& plan = *in.plan;
    const bool executed = in.stats != nullptr;

    // 1. Intent; 2. tables and columns.
    fact("intent", describeIntent(*in.statement));
    postOrder(plan, [&](const PhysicalNode& n) {
        if (!n.table || (n.op != PhysicalOp::SeqScan && n.op != PhysicalOp::IndexScan)) return;
        std::string cols;
        for (auto c : n.scanColumns) cols += (cols.empty() ? "" : ", ") + n.table->columns[c].name;
        fact("tables", "Table " + n.table->name + (n.alias != n.table->name ? " (as " + n.alias + ")" : "") + " has " +
                           num(static_cast<double>(n.table->rowCount())) + " rows in " + num(static_cast<double>(n.table->pageCount())) +
                           " pages; the query reads " + std::to_string(n.scanColumns.size()) + " of its " +
                           std::to_string(n.table->columns.size()) + " columns" + (cols.empty() ? "" : " (" + cols + ")") + ".",
             n.id);
    });

    // 3. Filtering, measured.
    postOrder(plan, [&](const PhysicalNode& n) {
        const auto* s = statOf(in, n.id);
        if (n.op == PhysicalOp::SeqScan) {
            if (!s) {
                fact("filtering", "The engine will read every page of " + n.table->name +
                                      (n.predicate ? " and test " + n.predicate->toString() + " on each row." : "."),
                     n.id);
                return;
            }
            std::string t = "The sequential scan of " + n.table->name + " read " + num(static_cast<double>(s->pagesRead)) + " pages (" +
                            num(static_cast<double>(s->pageMisses)) + " not in the buffer pool) and examined " +
                            num(static_cast<double>(s->rowsScanned)) + " rows";
            if (n.predicate) {
                t += "; the filter " + n.predicate->toString() + " kept " + num(static_cast<double>(s->rowsOut)) + " (" +
                     pct(static_cast<double>(s->rowsOut), static_cast<double>(s->rowsScanned)) + ") and discarded " +
                     num(static_cast<double>(s->rowsFiltered));
            }
            fact("filtering", t + ".", n.id);
        } else if (n.op == PhysicalOp::IndexScan) {
            if (!s) {
                fact("filtering", "The engine will look up " + n.bounds.describe(*n.table) + " in index " + n.bounds.index->name + ".", n.id);
                return;
            }
            std::string t = "The index scan on " + n.bounds.index->name + " (" + n.bounds.describe(*n.table) + ") visited " +
                            num(static_cast<double>(s->indexNodesVisited)) + " B+ tree nodes, examined " +
                            num(static_cast<double>(s->indexEntriesExamined)) + " index entries and fetched " +
                            num(static_cast<double>(s->rowsScanned)) + " rows from " + num(static_cast<double>(s->pagesRead)) +
                            " heap page accesses, instead of reading all " + num(static_cast<double>(n.table->rowCount())) + " rows";
            if (n.predicate) {
                t += "; the remaining filter " + n.predicate->toString() + " kept " + num(static_cast<double>(s->rowsOut));
            }
            fact("filtering", t + ".", n.id);
        } else if (n.op == PhysicalOp::Filter && s) {
            fact("filtering", "The filter " + n.predicate->toString() + " kept " + num(static_cast<double>(s->rowsOut)) + " of " +
                                  num(static_cast<double>(s->rowsIn)) + " rows.",
                 n.id);
        }
    });

    // 4. Index / access-path decisions and 5. operator choices.
    if (in.trace) {
        for (const auto& step : *in.trace) {
            if (step.phase != "physical") continue;
            std::string t = step.reason;
            if (step.rule == "AccessPathSelection") {
                std::string considered;
                for (const auto& a : step.alternatives) {
                    if (a.name.rfind("Index", 0) != 0) continue;
                    considered += (considered.empty() ? "" : "; ") + a.name + (a.cost > 0 ? " (est. cost " + num(a.cost, 1) + ")" : " — " + a.detail);
                }
                if (!considered.empty()) t += " Considered: " + considered + ".";
                fact("indexes", t, step.nodeIds.empty() ? 0 : step.nodeIds[0]);
            } else {
                fact("operators", t, step.nodeIds.empty() ? 0 : step.nodeIds[0]);
            }
        }
    }
    std::string ops;
    postOrder(plan, [&](const PhysicalNode& n) { ops += (ops.empty() ? "" : " → ") + opShort(n); });
    fact("operators", "Physical operators, in the order they produce rows: " + ops + ".");

    // 6. Row flow; 7. hotspot.
    json hotspot = nullptr;
    if (executed) {
        std::string flow;
        postOrder(plan, [&](const PhysicalNode& n) {
            const auto* s = statOf(in, n.id);
            if (!s) return;
            flow += (flow.empty() ? "" : " → ") + opShort(n) + ": " + num(static_cast<double>(s->rowsOut));
        });
        fact("rowflow", "Rows produced by each operator: " + flow + ". The query returned " + num(static_cast<double>(in.resultRows)) + " rows.");

        const PhysicalNode* hot = nullptr;
        double hotSelf = -1, total = 0;
        postOrder(plan, [&](const PhysicalNode& n) {
            const auto* s = statOf(in, n.id);
            if (!s) return;
            std::int64_t childNs = 0;
            for (const auto& c : n.children) {
                if (const auto* cs = statOf(in, c->id)) childNs += cs->inclusiveNs;
            }
            const double self = static_cast<double>(std::max<std::int64_t>(0, s->inclusiveNs - childNs));
            total += self;
            if (self > hotSelf) {
                hotSelf = self;
                hot = &n;
            }
        });
        if (hot && total > 0) {
            const auto* s = statOf(in, hot->id);
            std::string why;
            if (s && s->pagesRead > 0) why = " It read " + num(static_cast<double>(s->pagesRead)) + " pages.";
            if (s && s->buildRows > 0) why += " It buffered " + num(static_cast<double>(s->buildRows)) + " rows.";
            fact("hotspot", "Most measured time was spent in " + opShort(*hot) + ": " + num(hotSelf / 1e6, 3) + " ms of " +
                                num(total / 1e6, 3) + " ms of operator time (" + pct(hotSelf, total) + ")." + why,
                 hot->id);
            hotspot = {{"nodeId", hot->id}, {"op", opShort(*hot)}, {"selfTimeMs", hotSelf / 1e6}, {"share", hotSelf / total}};
        }

        // Estimated vs actual cardinality.
        postOrder(plan, [&](const PhysicalNode& n) {
            const auto* s = statOf(in, n.id);
            if (!s || !s->opened) return;
            const double est = n.est.rows, act = static_cast<double>(s->rowsOut);
            // A LIMIT above can stop an operator early; compare only completed ones.
            if (!s->exhausted) return;
            const double hi = std::max(est, act), lo = std::max(1.0, std::min(est, act));
            if (hi / lo >= 2.0 && std::fabs(est - act) >= 20) {
                warnings.push_back({{"nodeId", n.id}, {"op", opShort(n)}, {"estimatedRows", est}, {"actualRows", act}, {"factor", hi / lo}});
                fact("estimates", "The optimizer estimated " + num(est) + " rows for " + opShort(n) + " but it produced " + num(act) +
                                      " (off by " + num(hi / lo, 1) + "×). Cardinality estimates rely on simplified statistics "
                                      "(uniform distribution per value, independence between predicates), so plans built on them can be suboptimal.",
                     n.id);
            }
        });

        if (in.timerCalls > 0 && in.timerOverheadNs > 0) {
            const double overheadMs = static_cast<double>(in.timerCalls) * in.timerOverheadNs / 1e6;
            fact("timing", "Engine execution took " + num(in.executeMs, 3) + " ms (excluding parsing, planning, JSON encoding and network). "
                           "Per-operator timers were read " + num(static_cast<double>(in.timerCalls)) + " times at ~" +
                               num(in.timerOverheadNs, 0) + " ns each, so roughly " + num(overheadMs, 3) +
                               " ms of the measured time is instrumentation overhead.");
            if (in.executeMs < 1.0) {
                guide("This query ran in under a millisecond. At this scale timings are dominated by noise and timer overhead; "
                      "compare pages read and rows examined instead, or use the Benchmarks tab, which repeats the query and reports medians.");
            }
        }
    }

    // 8. Experiments and general guidance.
    bool seqWithFilter = false, hasSort = false, hasNL = false, hasIndex = false, hasLimitOverSort = false;
    postOrder(plan, [&](const PhysicalNode& n) {
        if (n.op == PhysicalOp::SeqScan && n.predicate) seqWithFilter = true;
        if (n.op == PhysicalOp::Sort) hasSort = true;
        if (n.op == PhysicalOp::NestedLoopJoin) hasNL = true;
        if (n.op == PhysicalOp::IndexScan) hasIndex = true;
        if (n.op == PhysicalOp::Limit && !n.children.empty() && n.children[0]->op == PhysicalOp::Sort) hasLimitOverSort = true;
    });
    if (seqWithFilter) {
        guide("Indexes help most when a filter keeps a small fraction of a large table. When most rows match, a sequential scan is "
              "usually cheaper because it reads each page once, in order.");
    }
    if (hasIndex) {
        guide("Try the same query in the Performance Lab with index scans disabled to see the work a sequential scan would do.");
    }
    if (hasSort) {
        guide("Sort must consume its entire input before returning the first row." +
              std::string(hasLimitOverSort ? " Here LIMIT sits above Sort, so it reduces the rows returned but not the rows sorted." : ""));
    }
    if (hasNL) {
        guide("Nested loop joins compare every pair of rows. They are chosen when the join condition has no column = column "
              "equality (so a hash join is impossible) or when inputs are tiny.");
    }

    std::string summary;
    if (executed) {
        std::uint64_t pages = 0, scanned = 0;
        postOrder(plan, [&](const PhysicalNode& n) {
            if (const auto* s = statOf(in, n.id)) {
                pages += s->pagesRead;
                scanned += s->rowsScanned;
            }
        });
        summary = "Returned " + num(static_cast<double>(in.resultRows)) + " rows in " + num(in.executeMs, 3) + " ms after examining " +
                  num(static_cast<double>(scanned)) + " stored rows across " + num(static_cast<double>(pages)) + " page accesses.";
    } else {
        summary = "Plan only (not executed): estimated " + num(plan.est.rows) + " output rows at cost " + num(plan.est.totalCost, 1) + ".";
    }
    return json{{"summary", summary}, {"facts", facts}, {"guidance", guidance}, {"estimateWarnings", warnings}, {"hotspot", hotspot}};
}

json adviseIndexes(const InsightInput& in) {
    json suggestions = json::array();
    json notes = json::array();
    std::set<std::string> seen;
    const auto& cost = *in.cost;

    postOrder(*in.plan, [&](const PhysicalNode& n) {
        if (n.op == PhysicalOp::HashJoin || n.op == PhysicalOp::NestedLoopJoin) {
            notes.push_back({{"kind", "join"},
                             {"nodeId", n.id},
                             {"text", "QueryLab joins with hash or nested-loop joins and never uses an index to look up join partners, "
                                      "so an index on the join key would not change this join. (Engines with index nested-loop joins, "
                                      "such as PostgreSQL, can benefit.)"}});
        }
        if (n.op != PhysicalOp::SeqScan || !n.predicate) return;
        const auto& t = *n.table;
        std::vector<const BoundExpr*> terms;
        conjuncts(n.predicate.get(), terms);

        struct Cand {
            std::size_t pos;
            std::string pred;
            bool equality;
            const BoundExpr* expr;
        };
        std::vector<Cand> cands;
        bool unindexable = false;
        const auto layout = planner::tableLayout(n);
        auto posOf = [&](const BoundExpr& c) -> int {
            if (c.kind != BoundKind::Column || c.slot < 0) return -1;
            return c.slot;  // scan predicates are resolved to table positions
        };
        for (const auto* e : terms) {
            if (e->kind == BoundKind::Binary && (e->op == "=" || e->op == "<" || e->op == "<=" || e->op == ">" || e->op == ">=")) {
                const BoundExpr* col = nullptr;
                if (e->children[0]->kind == BoundKind::Column && e->children[1]->kind == BoundKind::Literal) col = e->children[0].get();
                if (e->children[1]->kind == BoundKind::Column && e->children[0]->kind == BoundKind::Literal) col = e->children[1].get();
                if (col && posOf(*col) >= 0) {
                    cands.push_back({static_cast<std::size_t>(posOf(*col)), e->toString(), e->op == "=", e});
                    continue;
                }
            } else if (e->kind == BoundKind::Between && !e->negated && posOf(*e->children[0]) >= 0) {
                cands.push_back({static_cast<std::size_t>(posOf(*e->children[0])), e->toString(), false, e});
                continue;
            }
            unindexable = true;
        }
        (void)layout;
        if (cands.empty()) {
            notes.push_back({{"kind", "unindexable"},
                             {"nodeId", n.id},
                             {"text", "The filter on " + t.name + " (" + n.predicate->toString() +
                                          ") has no `column = constant` or range condition; QueryLab indexes cannot answer OR, IN, LIKE, "
                                          "function or column-to-column conditions."}});
            return;
        }
        // Existing index already usable?
        for (const auto* idx : t.indexes) {
            for (const auto& c : cands) {
                if (idx->columns[0] == c.pos) {
                    notes.push_back({{"kind", "existing-index"},
                                     {"nodeId", n.id},
                                     {"text", "Index " + idx->name + " on " + t.name + " could answer " + c.pred +
                                                  ", but the optimizer estimated a sequential scan to be cheaper for this predicate. "
                                                  "Adding another index would not change that decision."}});
                    return;
                }
            }
        }
        const auto* s = statOf(in, n.id);
        const double scanned = s ? static_cast<double>(s->rowsScanned) : static_cast<double>(t.rowCount());
        const double kept = s ? static_cast<double>(s->rowsOut) : n.est.rows;
        const double sel = scanned > 0 ? kept / scanned : 1.0;
        if (t.rowCount() < 200) {
            notes.push_back({{"kind", "small-table"},
                             {"nodeId", n.id},
                             {"text", t.name + " has only " + num(static_cast<double>(t.rowCount())) + " rows (" +
                                          num(static_cast<double>(t.pageCount())) + " pages); scanning it is already cheap, so no index is suggested."}});
            return;
        }
        if (sel > 0.2) {
            notes.push_back({{"kind", "unselective"},
                             {"nodeId", n.id},
                             {"text", "The filter on " + t.name + " kept " + pct(kept, scanned) +
                                          " of rows. With that many matches an index scan would fetch most pages anyway, so the optimizer "
                                          "would likely keep the sequential scan. No index suggested."}});
            return;
        }

        // Equality columns first (most distinct first), then one range column.
        std::vector<Cand> eqs, ranges;
        for (const auto& c : cands) (c.equality ? eqs : ranges).push_back(c);
        std::sort(eqs.begin(), eqs.end(), [&](const Cand& a, const Cand& b) { return t.stats[a.pos].distinct() > t.stats[b.pos].distinct(); });
        std::vector<Cand> chosen;
        std::set<std::size_t> used;
        for (const auto& c : eqs) {
            if (chosen.size() < 3 && used.insert(c.pos).second) chosen.push_back(c);
        }
        if (!ranges.empty() && chosen.size() < 3 && !used.count(ranges[0].pos)) chosen.push_back(ranges[0]);

        std::vector<std::string> colNames;
        std::string key = t.name + ":";
        for (const auto& c : chosen) {
            colNames.push_back(t.columns[c.pos].name);
            key += t.columns[c.pos].name + ",";
        }
        if (!seen.insert(key).second) return;
        std::string idxName = "idx_" + t.name;
        for (const auto& c : colNames) idxName += "_" + c;
        std::string def = "CREATE INDEX " + idxName + " ON " + t.name + " (";
        for (std::size_t i = 0; i < colNames.size(); ++i) def += (i ? ", " : "") + colNames[i];
        def += ")";

        const double rows = static_cast<double>(t.rowCount());
        const double pages = static_cast<double>(t.pageCount());
        const double height = std::max(1.0, std::ceil(std::log(std::max(rows, 2.0)) / std::log(32.0)));
        const double colFrac = static_cast<double>(n.scanColumns.size()) / static_cast<double>(t.columns.size());
        std::size_t residual = terms.size() > chosen.size() ? terms.size() - chosen.size() : 0;
        const double current = cost.seqScanCost(pages, rows, terms.size(), colFrac);
        // Rows the index would have to fetch: rows matching the indexed predicates.
        // Measured when the index would answer the whole filter; otherwise the
        // cost model's estimate for the indexed conditions alone.
        double matched = kept;
        if (residual > 0 || !s) {
            double sel2 = 1.0;
            for (const auto& c : chosen) sel2 *= cost.selectivity(*c.expr);
            matched = std::max(kept, rows * sel2);
        }
        const double withIdx = cost.indexScanCost(height, matched, pages, residual, colFrac);

        double keyBytes = 0;
        for (const auto& c : chosen) keyBytes += t.columns[c.pos].type == DataType::Text ? 24.0 : 9.0;
        const double approxBytes = rows * (keyBytes + 24.0);

        json evidence = json::array();
        if (s) {
            evidence.push_back("Seq Scan on " + t.name + " examined " + num(scanned) + " rows and kept " + num(kept) + " (" + pct(kept, scanned) + ").");
            evidence.push_back("It read " + num(static_cast<double>(s->pagesRead)) + " pages to find them.");
        } else {
            evidence.push_back("Plan reads all " + num(rows) + " rows of " + t.name + " to evaluate " + n.predicate->toString() + ".");
        }
        for (const auto& c : chosen) evidence.push_back("Indexable condition: " + c.pred + (c.equality ? " (equality)" : " (range)"));
        if (in.queryFrequency > 1) evidence.push_back("This query shape ran " + std::to_string(in.queryFrequency) + " times in this session.");

        json limitations = json::array();
        if (unindexable) limitations.push_back("Some conditions (OR / IN / LIKE / expressions) stay as a residual filter after the index lookup.");
        if (chosen.size() > 1) limitations.push_back("A composite index helps only when its leading column(s) are constrained; queries filtering only on a later column cannot use it.");
        limitations.push_back("QueryLab does not use index order to avoid sorts or for GROUP BY, so ORDER BY / GROUP BY columns are not included.");
        limitations.push_back("The benefit shown is a cost-model estimate, not a measurement. The optimizer may still prefer a sequential scan if statistics change.");

        suggestions.push_back({{"table", t.name},
                               {"columns", colNames},
                               {"definition", def},
                               {"nodeId", n.id},
                               {"evidence", evidence},
                               {"expectedBenefit", "Estimated scan cost would drop from " + num(current, 1) + " to about " + num(withIdx, 1) +
                                                       " cost units by fetching ~" + num(matched) + " rows through the index instead of reading " +
                                                       num(pages) + " pages. Measure before relying on this."},
                               {"estimatedCost", {{"current", current}, {"withIndex", withIdx}}},
                               {"overhead", "Every INSERT, UPDATE and DELETE on " + t.name + " must also update this B+ tree (" + num(rows) +
                                                " entries, roughly " + num(approxBytes / 1024.0, 0) + " KB in memory)."},
                               {"limitations", limitations},
                               {"verify", json::array({"Run the query and note pages read / rows examined (or use Benchmarks).", def + ";",
                                                       "Rerun the same query and check that the plan shows an Index Scan using " + idxName + ".",
                                                       "Compare both runs side by side in the Performance Lab (median of several iterations)."})}});
    });

    // Sort hint, only as a note.
    postOrder(*in.plan, [&](const PhysicalNode& n) {
        if (n.op == PhysicalOp::Sort && in.stats) {
            if (const auto* s = statOf(in, n.id); s && s->buildRows > 10000) {
                notes.push_back({{"kind", "sort"},
                                 {"nodeId", n.id},
                                 {"text", "Sort buffered " + num(static_cast<double>(s->buildRows)) + " rows. QueryLab always sorts explicitly; a more selective "
                                          "filter (or LIMIT on a smaller input) reduces this work, an index does not."}});
            }
        }
    });
    return json{{"suggestions", suggestions}, {"notes", notes}};
}

}  // namespace querylab::explain

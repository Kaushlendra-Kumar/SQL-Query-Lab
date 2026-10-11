#include "querylab/planner/binder.h"

#include <algorithm>
#include <set>

namespace querylab::planner {

using sql::Expr;
using sql::ExprKind;

namespace {

[[noreturn]] void bindError(const std::string& msg, const SourceSpan& span) {
    throw QueryError(ErrorStage::Binder, msg, span);
}

bool comparable(DataType a, DataType b) {
    if (a == DataType::Null || b == DataType::Null) return true;
    if (isNumeric(a) && isNumeric(b)) return true;
    return a == b;
}

DataType numericResult(DataType a, DataType b) {
    if (a == DataType::Float || b == DataType::Float) return DataType::Float;
    return DataType::Integer;
}

bool isScalarFunction(const std::string& n) {
    return n == "lower" || n == "upper" || n == "length" || n == "abs" || n == "round" || n == "coalesce";
}

}  // namespace

// ---- helpers -------------------------------------------------------------------

LogicalPtr Binder::newNode(LogicalOp op) {
    auto n = std::make_unique<LogicalNode>();
    n->id = ids_.nextNode++;
    n->op = op;
    return n;
}

ColumnMeta Binder::newColumn(std::string name, DataType type, const BoundExpr* source) {
    ColumnMeta c;
    c.id = ids_.nextColumn++;
    c.name = std::move(name);
    c.type = type;
    if (source && source->kind == BoundKind::Column) {
        auto it = columnMeta_.find(source->column);
        if (it != columnMeta_.end()) {
            c.sourceTable = it->second.sourceTable;
            c.sourceColumn = it->second.sourceColumn;
        }
    }
    columnMeta_[c.id] = c;
    return c;
}

Binder::RangeVar Binder::makeRangeVar(const sql::TableRef& ref) {
    const catalog::TableInfo* t = catalog_.findTable(ref.name);
    if (!t) {
        std::string msg = "table '" + ref.name + "' does not exist";
        auto tables = catalog_.tables();
        if (!tables.empty()) {
            msg += " (available: ";
            for (std::size_t i = 0; i < tables.size() && i < 12; ++i) msg += (i ? ", " : "") + tables[i]->name;
            msg += ")";
        }
        bindError(msg, ref.span);
    }
    RangeVar rv;
    rv.visible = ref.visibleName();
    rv.table = t;
    for (std::size_t i = 0; i < t->columns.size(); ++i) {
        ColumnMeta c;
        c.id = ids_.nextColumn++;
        c.name = t->columns[i].name;
        c.qualifier = rv.visible;
        c.type = t->columns[i].type;
        c.sourceTable = t;
        c.sourceColumn = static_cast<int>(i);
        columnMeta_[c.id] = c;
        rv.columns.push_back(c);
    }
    return rv;
}

LogicalPtr Binder::scanNode(const RangeVar& rv) {
    auto n = newNode(LogicalOp::Scan);
    n->table = rv.table;
    n->alias = rv.visible;
    n->output = rv.columns;
    for (std::size_t i = 0; i < rv.columns.size(); ++i) n->scanColumns.push_back(i);
    return n;
}

void Binder::requireBoolean(const BoundExpr& e, const char* clause, const SourceSpan& span) {
    if (e.type != DataType::Boolean && e.type != DataType::Null) {
        bindError(std::string("argument of ") + clause + " must be BOOLEAN, not " + typeName(e.type), span);
    }
}

bool Binder::containsAggregate(const Expr& e) {
    if (e.kind == ExprKind::Function && isAggregateName(e.name)) return true;
    return std::any_of(e.children.begin(), e.children.end(), [](const auto& c) { return containsAggregate(*c); });
}

// ---- expression binding -------------------------------------------------------

BoundExprPtr Binder::bindColumn(const Expr& e, const Scope& scope) {
    const ColumnMeta* found = nullptr;
    if (!e.table.empty()) {
        const RangeVar* rv = nullptr;
        for (const auto* v : scope.vars) {
            if (v->visible == e.table) rv = v;
        }
        if (!rv) {
            std::string msg = "unknown table or alias '" + e.table + "'";
            for (const auto* v : scope.vars) {
                if (v->table->name == e.table && v->visible != e.table) {
                    msg += " (table '" + e.table + "' is aliased as '" + v->visible + "')";
                }
            }
            bindError(msg, e.span);
        }
        for (const auto& c : rv->columns) {
            if (c.name == e.column) found = &c;
        }
        if (!found) bindError("column '" + e.column + "' does not exist in '" + e.table + "'", e.span);
    } else {
        for (const auto* v : scope.vars) {
            for (const auto& c : v->columns) {
                if (c.name != e.column) continue;
                if (found) {
                    bindError("column reference '" + e.column + "' is ambiguous (it exists in '" + found->qualifier + "' and '" +
                                  c.qualifier + "')",
                              e.span);
                }
                found = &c;
            }
        }
        if (!found) {
            std::string msg = "column '" + e.column + "' does not exist";
            if (scope.vars.empty()) msg += " (the query has no FROM clause)";
            else if (scope.vars.size() == 1) msg += " in table '" + scope.vars[0]->table->name + "'";
            bindError(msg, e.span);
        }
    }
    auto b = makeColumn(*found);
    b->span = e.span;
    return b;
}

BoundExprPtr Binder::bindAggregateCall(const Expr& e, const Scope& scope, AggContext& agg) {
    if (!agg.allowAggregates) bindError("aggregate functions are not allowed here", e.span);
    AggregateCall call;
    call.func = e.name;
    call.distinct = e.distinct;
    DataType outType = DataType::Integer;
    if (e.starArg) {
        if (e.name != "count") bindError(e.name + "(*) is not supported; only COUNT(*)", e.span);
    } else {
        if (e.children.size() != 1) bindError("aggregate " + e.name + " takes exactly one argument", e.span);
        if (containsAggregate(*e.children[0])) bindError("aggregate function calls cannot be nested", e.children[0]->span);
        call.arg = bindExpr(*e.children[0], scope, nullptr, "aggregate argument");
        const DataType at = call.arg->type;
        if (e.name == "sum" || e.name == "avg") {
            if (!isNumeric(at) && at != DataType::Null) {
                bindError(e.name + " requires a numeric argument, got " + std::string(typeName(at)), e.children[0]->span);
            }
            outType = e.name == "avg" ? DataType::Float : (at == DataType::Float ? DataType::Float : DataType::Integer);
        } else if (e.name == "min" || e.name == "max") {
            outType = at == DataType::Null ? DataType::Integer : at;
        }
    }
    // Deduplicate identical aggregate calls.
    const std::string key = e.name + (e.distinct ? "!d" : "") + (call.arg ? call.arg->key() : "*");
    for (const auto& existing : *agg.aggregates) {
        const std::string k2 = existing.func + (existing.distinct ? "!d" : "") + (existing.arg ? existing.arg->key() : "*");
        if (k2 == key) {
            auto ref = makeColumn(existing.output);
            ref->span = e.span;
            return ref;
        }
    }
    call.output = newColumn(e.toString(), outType, nullptr);
    auto ref = makeColumn(call.output);
    ref->span = e.span;
    agg.aggregates->push_back(std::move(call));
    return ref;
}

BoundExprPtr Binder::bindExpr(const Expr& e, const Scope& scope, AggContext* agg, const char* clause) {
    if (!agg) {
        if (e.kind == ExprKind::Function && isAggregateName(e.name)) {
            bindError(std::string("aggregate functions are not allowed in ") + clause, e.span);
        }
        return bindNode(e, scope, nullptr, clause);
    }
    if (e.kind == ExprKind::Function && isAggregateName(e.name)) return bindAggregateCall(e, *agg->base, *agg);
    if (!containsAggregate(e)) {
        auto base = bindNode(e, *agg->base, nullptr, clause);
        const std::string key = base->key();
        for (const auto& [gk, col] : agg->groups) {
            if (gk == key) {
                auto ref = makeColumn(col);
                ref->span = e.span;
                return ref;
            }
        }
        if (base->isConstant()) return base;
        if (e.kind == ExprKind::ColumnRef) {
            bindError("column '" + e.toString() + "' must appear in the GROUP BY clause or be used in an aggregate function", e.span);
        }
    }
    return bindNode(e, scope, agg, clause);
}

BoundExprPtr Binder::bindNode(const Expr& e, const Scope& scope, AggContext* agg, const char* clause) {
    auto out = std::make_unique<BoundExpr>();
    out->span = e.span;
    auto child = [&](std::size_t i) { return bindExpr(*e.children[i], scope, agg, clause); };

    switch (e.kind) {
        case ExprKind::Literal:
            out->kind = BoundKind::Literal;
            out->literal = e.literal;
            out->type = e.literal.type();
            return out;
        case ExprKind::ColumnRef: return bindColumn(e, scope);
        case ExprKind::Star: bindError("'*' is only allowed in the SELECT list or COUNT(*)", e.span);
        case ExprKind::Unary: {
            out->kind = BoundKind::Unary;
            out->op = e.op;
            out->children.push_back(child(0));
            const DataType t = out->children[0]->type;
            if (e.op == "NOT") {
                requireBoolean(*out->children[0], "NOT", e.children[0]->span);
                out->type = DataType::Boolean;
            } else {
                if (!isNumeric(t) && t != DataType::Null) {
                    bindError("operator " + e.op + " requires a numeric operand, got " + typeName(t), e.span);
                }
                out->type = t == DataType::Null ? DataType::Integer : t;
            }
            return out;
        }
        case ExprKind::Binary: {
            out->kind = BoundKind::Binary;
            out->op = e.op;
            out->children.push_back(child(0));
            out->children.push_back(child(1));
            const DataType l = out->children[0]->type, r = out->children[1]->type;
            const std::string& op = e.op;
            if (op == "AND" || op == "OR") {
                requireBoolean(*out->children[0], op.c_str(), e.children[0]->span);
                requireBoolean(*out->children[1], op.c_str(), e.children[1]->span);
                out->type = DataType::Boolean;
            } else if (op == "=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=") {
                if (!comparable(l, r)) {
                    bindError(std::string("cannot compare ") + typeName(l) + " with " + typeName(r) + " using " + op, e.span);
                }
                out->type = DataType::Boolean;
            } else if (op == "||") {
                out->type = DataType::Text;
            } else {
                const bool ok = (isNumeric(l) || l == DataType::Null) && (isNumeric(r) || r == DataType::Null);
                if (!ok) {
                    bindError("operator " + op + " requires numeric operands, got " + typeName(l) + " and " + typeName(r) +
                                  (op == "+" && (l == DataType::Text || r == DataType::Text) ? " (use || to concatenate text)" : ""),
                              e.span);
                }
                out->type = numericResult(l == DataType::Null ? r : l, r == DataType::Null ? l : r);
                if (out->type == DataType::Null) out->type = DataType::Integer;
            }
            return out;
        }
        case ExprKind::IsNull:
            out->kind = BoundKind::IsNull;
            out->negated = e.negated;
            out->children.push_back(child(0));
            out->type = DataType::Boolean;
            return out;
        case ExprKind::Between:
            out->kind = BoundKind::Between;
            out->negated = e.negated;
            for (std::size_t i = 0; i < 3; ++i) out->children.push_back(child(i));
            for (std::size_t i = 1; i < 3; ++i) {
                if (!comparable(out->children[0]->type, out->children[i]->type)) {
                    bindError(std::string("cannot compare ") + typeName(out->children[0]->type) + " with " +
                                  typeName(out->children[i]->type) + " in BETWEEN",
                              e.span);
                }
            }
            out->type = DataType::Boolean;
            return out;
        case ExprKind::InList:
            out->kind = BoundKind::InList;
            out->negated = e.negated;
            for (std::size_t i = 0; i < e.children.size(); ++i) {
                out->children.push_back(child(i));
                if (i > 0 && !comparable(out->children[0]->type, out->children[i]->type)) {
                    bindError(std::string("cannot compare ") + typeName(out->children[0]->type) + " with " +
                                  typeName(out->children[i]->type) + " in IN list",
                              e.children[i]->span);
                }
            }
            out->type = DataType::Boolean;
            return out;
        case ExprKind::Like:
            out->kind = BoundKind::Like;
            out->negated = e.negated;
            out->children.push_back(child(0));
            out->children.push_back(child(1));
            for (int i = 0; i < 2; ++i) {
                const DataType t = out->children[static_cast<std::size_t>(i)]->type;
                if (t != DataType::Text && t != DataType::Null) bindError(std::string("LIKE requires TEXT operands, got ") + typeName(t), e.span);
            }
            out->type = DataType::Boolean;
            return out;
        case ExprKind::Function: {
            if (isAggregateName(e.name)) bindError("aggregate functions are not allowed here", e.span);
            if (!isScalarFunction(e.name)) {
                bindError("function '" + e.name + "' does not exist (supported: COUNT, SUM, AVG, MIN, MAX, LOWER, UPPER, LENGTH, ABS, ROUND, COALESCE)",
                          e.span);
            }
            if (e.starArg || e.distinct) bindError("'*' and DISTINCT are only valid in aggregate functions", e.span);
            out->kind = BoundKind::Function;
            out->func = e.name;
            for (std::size_t i = 0; i < e.children.size(); ++i) out->children.push_back(child(i));
            const auto argc = out->children.size();
            auto argType = [&](std::size_t i) { return out->children[i]->type; };
            const std::string& f = e.name;
            if (f == "coalesce") {
                if (argc == 0) bindError("COALESCE needs at least one argument", e.span);
                DataType t = DataType::Null;
                for (std::size_t i = 0; i < argc; ++i) {
                    const DataType a = argType(i);
                    if (a == DataType::Null) continue;
                    if (t == DataType::Null) t = a;
                    else if (isNumeric(t) && isNumeric(a)) t = numericResult(t, a);
                    else if (t != a) bindError(std::string("COALESCE arguments have incompatible types ") + typeName(t) + " and " + typeName(a), e.span);
                }
                out->type = t;
                return out;
            }
            const std::size_t maxArgs = f == "round" ? 2 : 1;
            if (argc < 1 || argc > maxArgs) bindError("wrong number of arguments to " + f, e.span);
            const DataType a0 = argType(0);
            if (f == "lower" || f == "upper" || f == "length") {
                if (a0 != DataType::Text && a0 != DataType::Null) bindError(f + " requires a TEXT argument, got " + typeName(a0), e.span);
                out->type = f == "length" ? DataType::Integer : DataType::Text;
            } else {
                if (!isNumeric(a0) && a0 != DataType::Null) bindError(f + " requires a numeric argument, got " + typeName(a0), e.span);
                if (f == "round" && argc == 2 && argType(1) != DataType::Integer && argType(1) != DataType::Null) {
                    bindError("ROUND precision must be an INTEGER", e.children[1]->span);
                }
                out->type = f == "round" ? DataType::Float : (a0 == DataType::Null ? DataType::Integer : a0);
            }
            return out;
        }
    }
    bindError("unsupported expression", e.span);
}

// ---- SELECT -----------------------------------------------------------------------

LogicalPtr Binder::bindSelect(const sql::SelectStmt& s) {
    Scope scope;
    LogicalPtr root;

    if (s.from) {
        rangeVars_.push_back(std::make_unique<RangeVar>(makeRangeVar(*s.from)));
        scope.vars.push_back(rangeVars_.back().get());
        root = scanNode(*rangeVars_.back());
        for (const auto& j : s.joins) {
            for (const auto* v : scope.vars) {
                if (v->visible == j.table.visibleName()) {
                    bindError("table name '" + v->visible + "' specified more than once (use an alias)", j.table.span);
                }
            }
            rangeVars_.push_back(std::make_unique<RangeVar>(makeRangeVar(j.table)));
            scope.vars.push_back(rangeVars_.back().get());
            auto join = newNode(LogicalOp::Join);
            join->joinType = j.type;
            auto right = scanNode(*rangeVars_.back());
            if (j.on) {
                // ON may reference tables joined so far (including this one).
                join->predicate = bindExpr(*j.on, scope, nullptr, "JOIN ON");
                requireBoolean(*join->predicate, "JOIN ON", j.on->span);
            }
            join->output = root->output;
            join->output.insert(join->output.end(), right->output.begin(), right->output.end());
            join->children.push_back(std::move(root));
            join->children.push_back(std::move(right));
            root = std::move(join);
        }
    } else {
        root = newNode(LogicalOp::Values);
    }

    if (s.where) {
        auto f = newNode(LogicalOp::Filter);
        f->predicate = bindExpr(*s.where, scope, nullptr, "WHERE");
        requireBoolean(*f->predicate, "WHERE", s.where->span);
        f->output = root->output;
        f->children.push_back(std::move(root));
        root = std::move(f);
    }

    // Expand the select list (stars) into concrete items.
    struct Item {
        const Expr* expr = nullptr;  // nullptr for star-expanded columns
        ColumnMeta starColumn;
        std::string name;
        bool aliased = false;
        SourceSpan span;
    };
    std::vector<Item> items;
    for (const auto& it : s.items) {
        if (it.expr->kind == ExprKind::Star) {
            if (!it.alias.empty()) bindError("'*' cannot have an alias", it.span);
            bool matched = false;
            for (const auto* v : scope.vars) {
                if (!it.expr->table.empty() && v->visible != it.expr->table) continue;
                matched = true;
                for (const auto& c : v->columns) items.push_back(Item{nullptr, c, c.name, false, it.span});
            }
            if (!matched) {
                bindError(scope.vars.empty() ? "SELECT * requires a FROM clause" : "unknown table or alias '" + it.expr->table + "'",
                          it.expr->span);
            }
            continue;
        }
        Item item;
        item.expr = it.expr.get();
        item.span = it.span;
        if (!it.alias.empty()) {
            item.name = it.alias;
            item.aliased = true;
        } else if (it.expr->kind == ExprKind::ColumnRef) {
            item.name = it.expr->column;
        } else {
            item.name = it.expr->toString();
        }
        items.push_back(std::move(item));
    }

    // Aggregation?
    bool aggregating = !s.groupBy.empty() || (s.having != nullptr);
    for (const auto& it : s.items) aggregating = aggregating || containsAggregate(*it.expr);
    for (const auto& o : s.orderBy) aggregating = aggregating || containsAggregate(*o.expr);

    std::vector<AggregateCall> aggregates;
    AggContext aggCtx;
    aggCtx.base = &scope;
    aggCtx.aggregates = &aggregates;
    AggContext* agg = nullptr;
    LogicalNode* aggNode = nullptr;

    // Resolve a GROUP BY / ORDER BY reference to an output alias or ordinal.
    auto selectItemFor = [&](const Expr& e, const char* clause) -> const Item* {
        if (e.kind == ExprKind::Literal && e.literal.type() == DataType::Integer) {
            const auto n = e.literal.asInt();
            if (n < 1 || static_cast<std::size_t>(n) > items.size()) {
                bindError(std::string(clause) + " position " + std::to_string(n) + " is not in the select list", e.span);
            }
            return &items[static_cast<std::size_t>(n - 1)];
        }
        if (e.kind == ExprKind::ColumnRef && e.table.empty()) {
            const Item* match = nullptr;
            for (const auto& it : items) {
                if (it.aliased && it.name == e.column) {
                    if (match) bindError(std::string(clause) + " reference '" + e.column + "' is ambiguous", e.span);
                    match = &it;
                }
            }
            return match;
        }
        return nullptr;
    };

    if (aggregating) {
        auto a = newNode(LogicalOp::Aggregate);
        for (const auto& g : s.groupBy) {
            const Expr* ge = g.get();
            if (const Item* it = selectItemFor(*g, "GROUP BY")) {
                // Output alias / ordinal, unless it names a real input column.
                bool isInputColumn = false;
                if (g->kind == ExprKind::ColumnRef) {
                    for (const auto* v : scope.vars) {
                        for (const auto& c : v->columns) isInputColumn = isInputColumn || c.name == g->column;
                    }
                }
                if (!isInputColumn) {
                    if (!it->expr) {
                        auto col = makeColumn(it->starColumn);
                        const std::string key = col->key();
                        ColumnMeta out = newColumn(it->starColumn.name, col->type, col.get());
                        out.qualifier = it->starColumn.qualifier;
                        aggCtx.groups.emplace_back(key, out);
                        a->groupBy.push_back(std::move(col));
                        a->output.push_back(out);
                        continue;
                    }
                    ge = it->expr;
                }
            }
            if (containsAggregate(*ge)) bindError("aggregate functions are not allowed in GROUP BY", ge->span);
            auto bound = bindExpr(*ge, scope, nullptr, "GROUP BY");
            const std::string key = bound->key();
            bool dup = false;
            for (const auto& [k, _] : aggCtx.groups) dup = dup || k == key;
            if (dup) continue;
            ColumnMeta out = newColumn(bound->kind == BoundKind::Column ? columnMeta_[bound->column].name : bound->toString(),
                                       bound->type, bound.get());
            if (bound->kind == BoundKind::Column) out.qualifier = columnMeta_[bound->column].qualifier;
            aggCtx.groups.emplace_back(key, out);
            a->groupBy.push_back(std::move(bound));
            a->output.push_back(out);
        }
        agg = &aggCtx;
        aggNode = a.get();
        a->children.push_back(std::move(root));
        root = std::move(a);
    }

    // Bind select items (in aggregate context when aggregating).
    std::vector<BoundExprPtr> projExprs;
    Schema projOutput;
    for (const auto& it : items) {
        BoundExprPtr b;
        if (!it.expr) {
            b = makeColumn(it.starColumn);
            if (agg) {
                const std::string key = b->key();
                bool found = false;
                for (const auto& [gk, col] : agg->groups) {
                    if (gk == key) {
                        b = makeColumn(col);
                        found = true;
                    }
                }
                if (!found) {
                    bindError("column '" + it.starColumn.displayName() + "' must appear in the GROUP BY clause or be used in an aggregate function",
                              it.span);
                }
            }
        } else {
            b = bindExpr(*it.expr, scope, agg, "SELECT list");
        }
        ColumnMeta out = newColumn(it.name, b->type == DataType::Null ? DataType::Integer : b->type, b.get());
        projOutput.push_back(out);
        projExprs.push_back(std::move(b));
    }

    if (s.having) {
        auto f = newNode(LogicalOp::Filter);
        f->predicate = bindExpr(*s.having, scope, agg, "HAVING");
        requireBoolean(*f->predicate, "HAVING", s.having->span);
        // Aggregate outputs are finalized below once all calls are known.
        f->children.push_back(std::move(root));
        root = std::move(f);
    }

    // ORDER BY: output column, ordinal, or expression (hidden column).
    std::vector<SortKey> sortKeys;
    std::size_t hidden = 0;
    for (const auto& o : s.orderBy) {
        const ColumnMeta* target = nullptr;
        if (const Item* it = selectItemFor(*o.expr, "ORDER BY")) {
            target = &projOutput[static_cast<std::size_t>(it - items.data())];
        }
        if (!target) {
            if (o.expr->kind == ExprKind::Literal && o.expr->literal.type() != DataType::Integer) {
                bindError("ORDER BY constant is not allowed; use a column, alias or position", o.expr->span);
            }
            auto b = bindExpr(*o.expr, scope, agg, "ORDER BY");
            const std::string key = b->key();
            for (std::size_t i = 0; i < projExprs.size() && !target; ++i) {
                if (projExprs[i]->key() == key) target = &projOutput[i];
            }
            if (!target) {
                if (s.distinct) bindError("for SELECT DISTINCT, ORDER BY expressions must appear in the select list", o.expr->span);
                ColumnMeta out = newColumn("__order" + std::to_string(hidden++), b->type, b.get());
                projOutput.push_back(out);
                projExprs.push_back(std::move(b));
                target = &projOutput.back();
            }
        }
        sortKeys.push_back(SortKey{makeColumn(*target), o.descending});
    }

    if (aggNode) {
        for (auto& call : aggregates) aggNode->output.push_back(call.output);
        aggNode->aggregates = std::move(aggregates);
        if (root.get() != aggNode) root->output = aggNode->output;  // HAVING filter
    }

    auto proj = newNode(LogicalOp::Project);
    proj->exprs = std::move(projExprs);
    proj->output = projOutput;
    proj->children.push_back(std::move(root));
    root = std::move(proj);

    if (s.distinct) {
        auto d = newNode(LogicalOp::Distinct);
        d->output = root->output;
        d->children.push_back(std::move(root));
        root = std::move(d);
    }
    if (!sortKeys.empty()) {
        auto so = newNode(LogicalOp::Sort);
        so->sortKeys = std::move(sortKeys);
        so->output = root->output;
        so->children.push_back(std::move(root));
        root = std::move(so);
    }
    if (s.limit || s.offset) {
        auto l = newNode(LogicalOp::Limit);
        l->limit = s.limit;
        l->offset = s.offset.value_or(0);
        l->output = root->output;
        l->children.push_back(std::move(root));
        root = std::move(l);
    }
    if (hidden > 0) {
        // Strip hidden ORDER BY columns.
        auto strip = newNode(LogicalOp::Project);
        const std::size_t visible = projOutput.size() - hidden;
        for (std::size_t i = 0; i < visible; ++i) {
            strip->exprs.push_back(makeColumn(projOutput[i]));
            strip->output.push_back(projOutput[i]);
        }
        strip->children.push_back(std::move(root));
        root = std::move(strip);
    }
    return root;
}

LogicalPtr Binder::bindTableFilter(const std::string& table, const SourceSpan& tableSpan, const sql::Expr* where) {
    sql::TableRef ref;
    ref.name = table;
    ref.span = tableSpan;
    rangeVars_.push_back(std::make_unique<RangeVar>(makeRangeVar(ref)));
    Scope scope;
    scope.vars.push_back(rangeVars_.back().get());
    LogicalPtr root = scanNode(*rangeVars_.back());
    if (where) {
        auto f = newNode(LogicalOp::Filter);
        f->predicate = bindExpr(*where, scope, nullptr, "WHERE");
        requireBoolean(*f->predicate, "WHERE", where->span);
        f->output = root->output;
        f->children.push_back(std::move(root));
        root = std::move(f);
    }
    return root;
}

BoundExprPtr Binder::bindAgainst(const sql::Expr& e, const Schema& schema, const std::string& qualifier) {
    RangeVar rv;
    rv.visible = qualifier;
    rv.columns = schema;
    rv.table = schema.empty() ? nullptr : schema.front().sourceTable;
    Scope scope;
    scope.vars.push_back(&rv);
    return bindExpr(e, scope, nullptr, "SET");
}

Value Binder::evalConstant(const sql::Expr& e) {
    Scope empty;
    BoundExprPtr b;
    try {
        b = bindExpr(e, empty, nullptr, "VALUES");
    } catch (const QueryError& err) {
        if (std::string(err.what()).find("does not exist") != std::string::npos && e.kind == ExprKind::ColumnRef) {
            bindError("VALUES must contain constant expressions; '" + e.toString() +
                          "' looks like a column name (did you mean the string '" + e.column + "'?)",
                      e.span);
        }
        throw;
    }
    return evaluate(*b, Row{});
}

}  // namespace querylab::planner

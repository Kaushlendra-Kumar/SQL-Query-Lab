#include "querylab/explain/ast_json.h"

namespace querylab::explain {

using namespace querylab::sql;

json spanToJson(const SourceSpan& s) {
    return json{{"begin", s.begin}, {"end", s.end}, {"line", s.line}, {"column", s.column}};
}

json valueToJson(const Value& v) {
    switch (v.type()) {
        case DataType::Null: return nullptr;
        case DataType::Boolean: return v.asBool();
        case DataType::Integer: return v.asInt();
        case DataType::Float: {
            const double d = v.asFloat();
            if (std::isfinite(d)) return d;
            return v.toString();  // JSON has no NaN/Infinity
        }
        case DataType::Text: return v.asText();
    }
    return nullptr;
}

json tokensToJson(const std::vector<Token>& tokens) {
    json arr = json::array();
    for (const auto& t : tokens) {
        arr.push_back({{"type", tokenTypeName(t.type)}, {"lexeme", t.lexeme}, {"value", t.value}, {"span", spanToJson(t.span)}});
    }
    return arr;
}

namespace {

class AstBuilder {
public:
    json node(const std::string& type, const std::string& label, const SourceSpan& span, json details = json::object()) {
        return json{{"id", nextId_++}, {"type", type}, {"label", label}, {"span", spanToJson(span)},
                    {"details", std::move(details)}, {"children", json::array()}};
    }

    /// A grouping node whose span covers its children.
    json clause(const std::string& type, const std::string& label, json children) {
        SourceSpan s{0, 0, 1, 1};
        bool first = true;
        for (const auto& c : children) {
            SourceSpan cs{c["span"]["begin"].get<std::size_t>(), c["span"]["end"].get<std::size_t>(),
                          c["span"]["line"].get<int>(), c["span"]["column"].get<int>()};
            if (first) { s = cs; first = false; }
            else {
                if (cs.begin < s.begin) { s.begin = cs.begin; s.line = cs.line; s.column = cs.column; }
                s.end = std::max(s.end, cs.end);
            }
        }
        json n = node(type, label, s);
        n["children"] = std::move(children);
        return n;
    }

    json expr(const Expr& e) {
        json details = json::object();
        std::string type = exprKindName(e.kind);
        std::string label;
        switch (e.kind) {
            case ExprKind::Literal:
                label = "Literal: " + e.literal.toSqlLiteral();
                details["value"] = valueToJson(e.literal);
                details["valueType"] = typeName(e.literal.type());
                break;
            case ExprKind::ColumnRef:
                label = "Column: " + e.toString();
                details["column"] = e.column;
                if (!e.table.empty()) details["qualifier"] = e.table;
                break;
            case ExprKind::Star:
                label = e.toString();
                if (!e.table.empty()) details["qualifier"] = e.table;
                break;
            case ExprKind::Unary:
            case ExprKind::Binary:
                label = std::string(e.kind == ExprKind::Unary ? "Unary: " : "Binary: ") + e.op;
                details["operator"] = e.op;
                break;
            case ExprKind::IsNull: label = e.negated ? "IS NOT NULL" : "IS NULL"; break;
            case ExprKind::Between: label = e.negated ? "NOT BETWEEN" : "BETWEEN"; break;
            case ExprKind::InList: label = e.negated ? "NOT IN" : "IN"; break;
            case ExprKind::Like: label = e.negated ? "NOT LIKE" : "LIKE"; break;
            case ExprKind::Function:
                label = "Function: " + e.toString();
                details["function"] = e.name;
                details["distinct"] = e.distinct;
                details["star"] = e.starArg;
                break;
        }
        details["sql"] = e.toString();
        json n = node(type, label, e.span, std::move(details));
        for (const auto& c : e.children) n["children"].push_back(expr(*c));
        return n;
    }

    json table(const TableRef& t) {
        json d{{"table", t.name}};
        if (!t.alias.empty()) d["alias"] = t.alias;
        return node("TableRef", "Table: " + t.name + (t.alias.empty() ? "" : " AS " + t.alias), t.span, d);
    }

    json select(const SelectStmt& s, const SourceSpan& span) {
        json root = node("SelectStatement", s.distinct ? "SELECT DISTINCT" : "SELECT", span, {{"distinct", s.distinct}});
        json items = json::array();
        for (const auto& it : s.items) {
            json e = expr(*it.expr);
            if (!it.alias.empty()) {
                json a = node("Alias", "AS " + it.alias, it.span, {{"alias", it.alias}});
                a["children"].push_back(std::move(e));
                items.push_back(std::move(a));
            } else {
                items.push_back(std::move(e));
            }
        }
        root["children"].push_back(clause("SelectList", "Projection", std::move(items)));
        if (s.from) {
            json from = json::array();
            from.push_back(table(*s.from));
            for (const auto& j : s.joins) {
                const char* jt = j.type == JoinType::Left ? "LEFT JOIN" : (j.on ? "INNER JOIN" : "CROSS JOIN");
                json jn = node("JoinClause", jt, j.span, {{"joinType", jt}});
                jn["children"].push_back(table(j.table));
                if (j.on) {
                    json on = clause("JoinCondition", "ON", json::array({expr(*j.on)}));
                    jn["children"].push_back(std::move(on));
                }
                from.push_back(std::move(jn));
            }
            root["children"].push_back(clause("From", "FROM", std::move(from)));
        }
        if (s.where) root["children"].push_back(clause("Where", "WHERE", json::array({expr(*s.where)})));
        if (!s.groupBy.empty()) {
            json g = json::array();
            for (const auto& e : s.groupBy) g.push_back(expr(*e));
            root["children"].push_back(clause("GroupBy", "GROUP BY", std::move(g)));
        }
        if (s.having) root["children"].push_back(clause("Having", "HAVING", json::array({expr(*s.having)})));
        if (!s.orderBy.empty()) {
            json o = json::array();
            for (const auto& item : s.orderBy) {
                json n = node("OrderItem", item.descending ? "DESC" : "ASC", item.span, {{"descending", item.descending}});
                n["children"].push_back(expr(*item.expr));
                o.push_back(std::move(n));
            }
            root["children"].push_back(clause("OrderBy", "ORDER BY", std::move(o)));
        }
        if (s.limit) {
            root["children"].push_back(node("Limit", "LIMIT " + std::to_string(*s.limit), s.limitSpan, {{"value", *s.limit}}));
        }
        if (s.offset) {
            root["children"].push_back(node("Offset", "OFFSET " + std::to_string(*s.offset), s.offsetSpan, {{"value", *s.offset}}));
        }
        return root;
    }

    json statement(const Statement& st) {
        json n = std::visit(
            [&](const auto& b) -> json {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, SelectStmt>) {
                    return select(b, st.span);
                } else if constexpr (std::is_same_v<T, CreateTableStmt>) {
                    json r = node("CreateTableStatement", "CREATE TABLE " + b.name, st.span, {{"table", b.name}});
                    for (const auto& c : b.columns) {
                        r["children"].push_back(node("ColumnDef", c.name + " " + typeName(c.type), c.span,
                                                     {{"column", c.name}, {"type", typeName(c.type)}}));
                    }
                    return r;
                } else if constexpr (std::is_same_v<T, CreateIndexStmt>) {
                    json cols = b.columns;
                    json r = node("CreateIndexStatement", std::string(b.unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ") + b.name,
                                  st.span, {{"index", b.name}, {"table", b.table}, {"columns", cols}, {"unique", b.unique}});
                    for (std::size_t i = 0; i < b.columns.size(); ++i) {
                        r["children"].push_back(node("ColumnRef", "Column: " + b.columns[i], b.columnSpans[i]));
                    }
                    return r;
                } else if constexpr (std::is_same_v<T, DropTableStmt>) {
                    return node("DropTableStatement", "DROP TABLE " + b.name, st.span, {{"table", b.name}});
                } else if constexpr (std::is_same_v<T, DropIndexStmt>) {
                    return node("DropIndexStatement", "DROP INDEX " + b.name, st.span, {{"index", b.name}});
                } else if constexpr (std::is_same_v<T, InsertStmt>) {
                    json r = node("InsertStatement", "INSERT INTO " + b.table, st.span, {{"table", b.table}, {"rowCount", b.rows.size()}});
                    for (std::size_t i = 0; i < b.rows.size(); ++i) {
                        json vals = json::array();
                        for (const auto& e : b.rows[i]) vals.push_back(expr(*e));
                        r["children"].push_back(clause("ValuesRow", "Row " + std::to_string(i + 1), std::move(vals)));
                    }
                    return r;
                } else if constexpr (std::is_same_v<T, UpdateStmt>) {
                    json r = node("UpdateStatement", "UPDATE " + b.table, st.span, {{"table", b.table}});
                    for (const auto& a : b.assignments) {
                        json an = node("Assignment", "SET " + a.column, SourceSpan::cover(a.span, a.value->span), {{"column", a.column}});
                        an["children"].push_back(expr(*a.value));
                        r["children"].push_back(std::move(an));
                    }
                    if (b.where) r["children"].push_back(clause("Where", "WHERE", json::array({expr(*b.where)})));
                    return r;
                } else {
                    json r = node("DeleteStatement", "DELETE FROM " + b.table, st.span, {{"table", b.table}});
                    if (b.where) r["children"].push_back(clause("Where", "WHERE", json::array({expr(*b.where)})));
                    return r;
                }
            },
            st.body);
        if (st.explain) n["details"]["explain"] = true;
        return n;
    }

private:
    int nextId_ = 0;
};

void renderText(const json& n, const std::string& prefix, bool last, bool root, std::string& out) {
    if (root) {
        out += n["label"].get<std::string>() + "\n";
    } else {
        out += prefix + (last ? "└── " : "├── ") + n["label"].get<std::string>() + "\n";
    }
    const auto& ch = n["children"];
    for (std::size_t i = 0; i < ch.size(); ++i) {
        renderText(ch[i], root ? "" : prefix + (last ? "    " : "│   "), i + 1 == ch.size(), false, out);
    }
}

}  // namespace

json statementToJson(const Statement& stmt) { return AstBuilder().statement(stmt); }

std::string astToText(const json& node) {
    std::string out;
    renderText(node, "", true, true, out);
    return out;
}

}  // namespace querylab::explain

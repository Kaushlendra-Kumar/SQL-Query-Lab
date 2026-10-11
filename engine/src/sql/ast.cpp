#include "querylab/sql/ast.h"

#include <cctype>

namespace querylab::sql {

const char* exprKindName(ExprKind k) {
    switch (k) {
        case ExprKind::Literal: return "Literal";
        case ExprKind::ColumnRef: return "ColumnRef";
        case ExprKind::Star: return "Star";
        case ExprKind::Unary: return "UnaryExpression";
        case ExprKind::Binary: return "BinaryExpression";
        case ExprKind::IsNull: return "IsNull";
        case ExprKind::Between: return "Between";
        case ExprKind::InList: return "InList";
        case ExprKind::Like: return "Like";
        case ExprKind::Function: return "FunctionCall";
    }
    return "?";
}

static std::string upperName(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string Expr::toString() const {
    switch (kind) {
        case ExprKind::Literal: return literal.toSqlLiteral();
        case ExprKind::ColumnRef: return table.empty() ? column : table + "." + column;
        case ExprKind::Star: return table.empty() ? "*" : table + ".*";
        case ExprKind::Unary:
            if (op == "NOT") return "NOT " + children[0]->toString();
            return op + children[0]->toString();
        case ExprKind::Binary: {
            auto side = [](const Expr& e) {
                const bool paren = e.kind == ExprKind::Binary || (e.kind == ExprKind::Unary && e.op == "NOT") ||
                                   e.kind == ExprKind::IsNull || e.kind == ExprKind::Between ||
                                   e.kind == ExprKind::InList || e.kind == ExprKind::Like;
                return paren ? "(" + e.toString() + ")" : e.toString();
            };
            return side(*children[0]) + " " + op + " " + side(*children[1]);
        }
        case ExprKind::IsNull: return children[0]->toString() + (negated ? " IS NOT NULL" : " IS NULL");
        case ExprKind::Between:
            return children[0]->toString() + (negated ? " NOT BETWEEN " : " BETWEEN ") + children[1]->toString() +
                   " AND " + children[2]->toString();
        case ExprKind::InList: {
            std::string s = children[0]->toString() + (negated ? " NOT IN (" : " IN (");
            for (std::size_t i = 1; i < children.size(); ++i) {
                if (i > 1) s += ", ";
                s += children[i]->toString();
            }
            return s + ")";
        }
        case ExprKind::Like:
            return children[0]->toString() + (negated ? " NOT LIKE " : " LIKE ") + children[1]->toString();
        case ExprKind::Function: {
            std::string s = upperName(name) + "(";
            if (starArg) return s + "*)";
            if (distinct) s += "DISTINCT ";
            for (std::size_t i = 0; i < children.size(); ++i) {
                if (i) s += ", ";
                s += children[i]->toString();
            }
            return s + ")";
        }
    }
    return "?";
}

std::unique_ptr<Expr> Expr::clone() const {
    auto e = std::make_unique<Expr>();
    e->kind = kind;
    e->span = span;
    e->literal = literal;
    e->table = table;
    e->column = column;
    e->op = op;
    e->name = name;
    e->negated = negated;
    e->distinct = distinct;
    e->starArg = starArg;
    for (const auto& c : children) e->children.push_back(c->clone());
    return e;
}

const char* Statement::kindName() const {
    switch (body.index()) {
        case 0: return "SELECT";
        case 1: return "CREATE TABLE";
        case 2: return "CREATE INDEX";
        case 3: return "DROP TABLE";
        case 4: return "DROP INDEX";
        case 5: return "INSERT";
        case 6: return "UPDATE";
        case 7: return "DELETE";
    }
    return "?";
}

}  // namespace querylab::sql

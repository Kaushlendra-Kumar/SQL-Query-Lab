#include "querylab/planner/bound_expr.h"

#include <cctype>
#include <cstdint>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace querylab::planner {

BoundExprPtr BoundExpr::clone() const {
    auto e = std::make_unique<BoundExpr>();
    e->kind = kind;
    e->type = type;
    e->literal = literal;
    e->column = column;
    e->columnName = columnName;
    e->slot = slot;
    e->op = op;
    e->func = func;
    e->negated = negated;
    e->span = span;
    for (const auto& c : children) e->children.push_back(c->clone());
    return e;
}

static bool needsParens(const BoundExpr& e) {
    return e.kind == BoundKind::Binary || (e.kind == BoundKind::Unary && e.op == "NOT") || e.kind == BoundKind::IsNull ||
           e.kind == BoundKind::Between || e.kind == BoundKind::InList || e.kind == BoundKind::Like;
}

std::string BoundExpr::toString() const {
    auto side = [](const BoundExpr& c) { return needsParens(c) ? "(" + c.toString() + ")" : c.toString(); };
    switch (kind) {
        case BoundKind::Literal: return literal.toSqlLiteral();
        case BoundKind::Column: return columnName;
        case BoundKind::Unary: return op == "NOT" ? "NOT " + side(*children[0]) : op + side(*children[0]);
        case BoundKind::Binary: return side(*children[0]) + " " + op + " " + side(*children[1]);
        case BoundKind::IsNull: return side(*children[0]) + (negated ? " IS NOT NULL" : " IS NULL");
        case BoundKind::Between:
            return side(*children[0]) + (negated ? " NOT BETWEEN " : " BETWEEN ") + side(*children[1]) + " AND " + side(*children[2]);
        case BoundKind::InList: {
            std::string s = side(*children[0]) + (negated ? " NOT IN (" : " IN (");
            for (std::size_t i = 1; i < children.size(); ++i) s += (i > 1 ? ", " : "") + children[i]->toString();
            return s + ")";
        }
        case BoundKind::Like: return side(*children[0]) + (negated ? " NOT LIKE " : " LIKE ") + side(*children[1]);
        case BoundKind::Function: {
            std::string s;
            for (char ch : func) s += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            s += "(";
            for (std::size_t i = 0; i < children.size(); ++i) s += (i ? ", " : "") + children[i]->toString();
            return s + ")";
        }
    }
    return "?";
}

std::string BoundExpr::key() const {
    std::string s = std::to_string(static_cast<int>(kind)) + "[";
    switch (kind) {
        case BoundKind::Literal: s += std::string(typeName(literal.type())) + ":" + literal.toSqlLiteral(); break;
        case BoundKind::Column: s += "#" + std::to_string(column); break;
        default: s += op + "|" + func + "|" + (negated ? "!" : ""); break;
    }
    for (const auto& c : children) s += c->key() + ",";
    return s + "]";
}

void BoundExpr::collectColumns(std::set<ColumnId>& out) const {
    if (kind == BoundKind::Column) out.insert(column);
    for (const auto& c : children) c->collectColumns(out);
}

bool BoundExpr::isConstant() const {
    if (kind == BoundKind::Column) return false;
    for (const auto& c : children) {
        if (!c->isConstant()) return false;
    }
    return true;
}

BoundExprPtr makeLiteral(Value v) {
    auto e = std::make_unique<BoundExpr>();
    e->kind = BoundKind::Literal;
    e->type = v.type();
    e->literal = std::move(v);
    return e;
}

BoundExprPtr makeColumn(const ColumnMeta& c) {
    auto e = std::make_unique<BoundExpr>();
    e->kind = BoundKind::Column;
    e->type = c.type;
    e->column = c.id;
    e->columnName = c.displayName();
    return e;
}

BoundExprPtr makeBinary(std::string op, BoundExprPtr l, BoundExprPtr r) {
    auto e = std::make_unique<BoundExpr>();
    e->kind = BoundKind::Binary;
    e->type = DataType::Boolean;
    e->op = std::move(op);
    e->span = SourceSpan::cover(l->span, r->span);
    e->children.push_back(std::move(l));
    e->children.push_back(std::move(r));
    return e;
}

std::vector<BoundExprPtr> splitConjuncts(BoundExprPtr e) {
    std::vector<BoundExprPtr> out;
    if (!e) return out;
    if (e->kind == BoundKind::Binary && e->op == "AND") {
        auto l = splitConjuncts(std::move(e->children[0]));
        auto r = splitConjuncts(std::move(e->children[1]));
        for (auto& x : l) out.push_back(std::move(x));
        for (auto& x : r) out.push_back(std::move(x));
        return out;
    }
    out.push_back(std::move(e));
    return out;
}

BoundExprPtr combineConjuncts(std::vector<BoundExprPtr> parts) {
    BoundExprPtr acc;
    for (auto& p : parts) {
        if (!p) continue;
        acc = acc ? makeBinary("AND", std::move(acc), std::move(p)) : std::move(p);
    }
    return acc;
}

void resolveSlots(BoundExpr& e, const Schema& layout) {
    if (e.kind == BoundKind::Column) {
        e.slot = -1;
        for (std::size_t i = 0; i < layout.size(); ++i) {
            if (layout[i].id == e.column) {
                e.slot = static_cast<int>(i);
                break;
            }
        }
        if (e.slot < 0) throw std::logic_error("column " + e.columnName + " (#" + std::to_string(e.column) + ") is not available in operator input");
    }
    for (auto& c : e.children) resolveSlots(*c, layout);
}

bool isAggregateName(const std::string& n) {
    return n == "count" || n == "sum" || n == "avg" || n == "min" || n == "max";
}

// ---- evaluation -------------------------------------------------------------

namespace {

// Overflow-checked 64-bit arithmetic (portable to MSVC).
bool addOverflow(std::int64_t a, std::int64_t b, std::int64_t* r) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, r);
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return true;
    *r = a + b;
    return false;
#endif
}
bool subOverflow(std::int64_t a, std::int64_t b, std::int64_t* r) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_sub_overflow(a, b, r);
#else
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return true;
    *r = a - b;
    return false;
#endif
}
bool mulOverflow(std::int64_t a, std::int64_t b, std::int64_t* r) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, r);
#else
    if (a != 0 && b != 0) {
        if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) return true;
        if (a > 0 ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a) : (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b)) return true;
    }
    *r = a * b;
    return false;
#endif
}

[[noreturn]] void runtimeError(const std::string& msg, const SourceSpan& span) {
    throw QueryError(ErrorStage::Executor, msg, span.end > span.begin ? std::optional<SourceSpan>(span) : std::nullopt);
}

/// SQL comparison: returns NULL if either side is NULL.
Value compareOp(const std::string& op, const Value& a, const Value& b) {
    if (a.isNull() || b.isNull()) return Value::null();
    const int c = Value::compareTotal(a, b);
    bool r;
    if (op == "=") r = c == 0;
    else if (op == "<>") r = c != 0;
    else if (op == "<") r = c < 0;
    else if (op == "<=") r = c <= 0;
    else if (op == ">") r = c > 0;
    else r = c >= 0;
    return Value::boolean(r);
}

Value arith(const std::string& op, const Value& a, const Value& b, const SourceSpan& span) {
    if (a.isNull() || b.isNull()) return Value::null();
    if (a.type() == DataType::Integer && b.type() == DataType::Integer) {
        const std::int64_t x = a.asInt(), y = b.asInt();
        std::int64_t r = 0;
        bool overflow = false;
        if (op == "+") overflow = addOverflow(x, y, &r);
        else if (op == "-") overflow = subOverflow(x, y, &r);
        else if (op == "*") overflow = mulOverflow(x, y, &r);
        else if (op == "/" || op == "%") {
            if (y == 0) runtimeError("division by zero", span);
            if (x == std::numeric_limits<std::int64_t>::min() && y == -1) overflow = true;
            else r = op == "/" ? x / y : x % y;
        }
        if (overflow) runtimeError("integer overflow in " + a.toString() + " " + op + " " + b.toString(), span);
        return Value::integer(r);
    }
    const double x = a.asNumber(), y = b.asNumber();
    if (op == "+") return Value::floating(x + y);
    if (op == "-") return Value::floating(x - y);
    if (op == "*") return Value::floating(x * y);
    if (y == 0) runtimeError("division by zero", span);
    if (op == "/") return Value::floating(x / y);
    return Value::floating(std::fmod(x, y));
}

}  // namespace


bool likeMatch(const std::string& text, const std::string& pattern) {
    // Iterative wildcard matching with backtracking on the last '%'.
    std::size_t t = 0, p = 0, starP = std::string::npos, starT = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '_' || pattern[p] == text[t])) {
            ++t;
            ++p;
        } else if (p < pattern.size() && pattern[p] == '%') {
            starP = p++;
            starT = t;
        } else if (starP != std::string::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '%') ++p;
    return p == pattern.size();
}

Value evaluate(const BoundExpr& e, const Row& row) {
    switch (e.kind) {
        case BoundKind::Literal: return e.literal;
        case BoundKind::Column: return row[static_cast<std::size_t>(e.slot)];
        case BoundKind::Unary: {
            Value v = evaluate(*e.children[0], row);
            if (v.isNull()) return v;
            if (e.op == "NOT") return Value::boolean(!v.asBool());
            if (e.op == "+") return v;
            if (v.type() == DataType::Integer) {
                if (v.asInt() == std::numeric_limits<std::int64_t>::min()) runtimeError("integer overflow in negation", e.span);
                return Value::integer(-v.asInt());
            }
            return Value::floating(-v.asFloat());
        }
        case BoundKind::Binary: {
            if (e.op == "AND") {
                Value l = evaluate(*e.children[0], row);
                if (!l.isNull() && !l.asBool()) return Value::boolean(false);  // short-circuit
                Value r = evaluate(*e.children[1], row);
                if (!r.isNull() && !r.asBool()) return Value::boolean(false);
                if (l.isNull() || r.isNull()) return Value::null();
                return Value::boolean(true);
            }
            if (e.op == "OR") {
                Value l = evaluate(*e.children[0], row);
                if (!l.isNull() && l.asBool()) return Value::boolean(true);
                Value r = evaluate(*e.children[1], row);
                if (!r.isNull() && r.asBool()) return Value::boolean(true);
                if (l.isNull() || r.isNull()) return Value::null();
                return Value::boolean(false);
            }
            Value l = evaluate(*e.children[0], row);
            Value r = evaluate(*e.children[1], row);
            if (e.op == "||") {
                if (l.isNull() || r.isNull()) return Value::null();
                return Value::text(l.toString() + r.toString());
            }
            if (e.op == "+" || e.op == "-" || e.op == "*" || e.op == "/" || e.op == "%") return arith(e.op, l, r, e.span);
            return compareOp(e.op, l, r);
        }
        case BoundKind::IsNull: {
            const bool isNull = evaluate(*e.children[0], row).isNull();
            return Value::boolean(e.negated ? !isNull : isNull);
        }
        case BoundKind::Between: {
            Value v = evaluate(*e.children[0], row);
            Value ge = compareOp(">=", v, evaluate(*e.children[1], row));
            Value le = compareOp("<=", v, evaluate(*e.children[2], row));
            Value r;
            if ((!ge.isNull() && !ge.asBool()) || (!le.isNull() && !le.asBool())) r = Value::boolean(false);
            else if (ge.isNull() || le.isNull()) r = Value::null();
            else r = Value::boolean(true);
            if (e.negated && !r.isNull()) r = Value::boolean(!r.asBool());
            return r;
        }
        case BoundKind::InList: {
            Value v = evaluate(*e.children[0], row);
            if (v.isNull()) return Value::null();
            bool sawNull = false;
            for (std::size_t i = 1; i < e.children.size(); ++i) {
                Value item = evaluate(*e.children[i], row);
                if (item.isNull()) {
                    sawNull = true;
                    continue;
                }
                if (Value::compareTotal(v, item) == 0) return Value::boolean(!e.negated);
            }
            if (sawNull) return Value::null();
            return Value::boolean(e.negated);
        }
        case BoundKind::Like: {
            Value v = evaluate(*e.children[0], row);
            Value p = evaluate(*e.children[1], row);
            if (v.isNull() || p.isNull()) return Value::null();
            const bool m = likeMatch(v.asText(), p.asText());
            return Value::boolean(e.negated ? !m : m);
        }
        case BoundKind::Function: {
            const std::string& f = e.func;
            if (f == "coalesce") {
                for (const auto& c : e.children) {
                    Value v = evaluate(*c, row);
                    if (!v.isNull()) return v;
                }
                return Value::null();
            }
            Value a = evaluate(*e.children[0], row);
            if (a.isNull()) return a;
            if (f == "lower" || f == "upper") {
                std::string s = a.asText();
                for (auto& ch : s) {
                    ch = static_cast<char>(f == "lower" ? std::tolower(static_cast<unsigned char>(ch))
                                                        : std::toupper(static_cast<unsigned char>(ch)));
                }
                return Value::text(std::move(s));
            }
            if (f == "length") return Value::integer(static_cast<std::int64_t>(a.asText().size()));
            if (f == "abs") {
                if (a.type() == DataType::Integer) {
                    if (a.asInt() == std::numeric_limits<std::int64_t>::min()) runtimeError("integer overflow in ABS", e.span);
                    return Value::integer(a.asInt() < 0 ? -a.asInt() : a.asInt());
                }
                return Value::floating(std::fabs(a.asFloat()));
            }
            if (f == "round") {
                std::int64_t digits = 0;
                if (e.children.size() > 1) {
                    Value d = evaluate(*e.children[1], row);
                    if (d.isNull()) return d;
                    digits = d.asInt();
                }
                if (digits < -15) digits = -15;
                if (digits > 15) digits = 15;
                const double scale = std::pow(10.0, static_cast<double>(digits));
                return Value::floating(std::round(a.asNumber() * scale) / scale);
            }
            throw std::logic_error("unknown scalar function " + f);
        }
    }
    throw std::logic_error("unhandled expression kind");
}

}  // namespace querylab::planner

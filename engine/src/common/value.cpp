#include "querylab/common/value.h"

#include <cmath>
#include <cstdio>
#include <limits>

#include "querylab/common/error.h"

namespace querylab {

const char* typeName(DataType t) {
    switch (t) {
        case DataType::Null: return "NULL";
        case DataType::Boolean: return "BOOLEAN";
        case DataType::Integer: return "INTEGER";
        case DataType::Float: return "FLOAT";
        case DataType::Text: return "TEXT";
    }
    return "?";
}

bool isNumeric(DataType t) { return t == DataType::Integer || t == DataType::Float; }

DataType Value::type() const {
    switch (v_.index()) {
        case 0: return DataType::Null;
        case 1: return DataType::Boolean;
        case 2: return DataType::Integer;
        case 3: return DataType::Float;
        default: return DataType::Text;
    }
}

double Value::asNumber() const {
    if (auto* i = std::get_if<std::int64_t>(&v_)) return static_cast<double>(*i);
    return std::get<double>(v_);
}

static std::string formatDouble(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.15g", d);
    std::string s(buf);
    // Keep a visible decimal point so floats are distinguishable from ints.
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

std::string Value::toString() const {
    switch (type()) {
        case DataType::Null: return "NULL";
        case DataType::Boolean: return asBool() ? "TRUE" : "FALSE";
        case DataType::Integer: return std::to_string(asInt());
        case DataType::Float: return formatDouble(asFloat());
        case DataType::Text: return asText();
    }
    return "";
}

std::string Value::toSqlLiteral() const {
    if (type() != DataType::Text) return toString();
    std::string out = "'";
    for (char c : asText()) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out + "'";
}

static int rank(DataType t) {
    switch (t) {
        case DataType::Null: return 0;
        case DataType::Boolean: return 1;
        case DataType::Integer:
        case DataType::Float: return 2;
        case DataType::Text: return 3;
    }
    return 4;
}

int Value::compareTotal(const Value& a, const Value& b) {
    const DataType ta = a.type(), tb = b.type();
    const int ra = rank(ta), rb = rank(tb);
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (ta) {
        case DataType::Null: return 0;
        case DataType::Boolean: return static_cast<int>(a.asBool()) - static_cast<int>(b.asBool());
        case DataType::Integer:
        case DataType::Float: {
            if (ta == DataType::Integer && tb == DataType::Integer) {
                return a.asInt() < b.asInt() ? -1 : (a.asInt() > b.asInt() ? 1 : 0);
            }
            const double x = a.asNumber(), y = b.asNumber();
            // NaN sorts after all other numbers so the order stays total.
            if (std::isnan(x) || std::isnan(y)) return std::isnan(x) - std::isnan(y);
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        case DataType::Text: {
            const int c = a.asText().compare(b.asText());
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
    }
    return 0;
}

std::size_t Value::hash() const {
    switch (type()) {
        case DataType::Null: return 0x9e3779b97f4a7c15ULL;
        case DataType::Boolean: return asBool() ? 1231 : 1237;
        case DataType::Integer: return std::hash<std::int64_t>{}(asInt());
        case DataType::Float: {
            const double d = asFloat();
            // Integral floats must hash like the equal integer (1.0 == 1).
            if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9.2e18) {
                return std::hash<std::int64_t>{}(static_cast<std::int64_t>(d));
            }
            return std::hash<double>{}(d);
        }
        case DataType::Text: return std::hash<std::string>{}(asText());
    }
    return 0;
}

std::size_t Value::footprint() const {
    std::size_t base = sizeof(Value);
    if (type() == DataType::Text) base += asText().capacity();
    return base;
}

std::size_t RowHash::operator()(const Row& r) const {
    std::size_t h = 1469598103934665603ULL;
    for (const auto& v : r) h = (h ^ v.hash()) * 1099511628211ULL;
    return h;
}

bool RowEq::operator()(const Row& a, const Row& b) const { return compareRows(a, b) == 0; }

int compareRows(const Row& a, const Row& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int c = Value::compareTotal(a[i], b[i]);
        if (c != 0) return c;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

Value coerceForStorage(const Value& v, DataType target, const std::string& columnName) {
    if (v.isNull() || v.type() == target) return v;
    if (target == DataType::Float && v.type() == DataType::Integer) {
        return Value::floating(static_cast<double>(v.asInt()));
    }
    if (target == DataType::Integer && v.type() == DataType::Float) {
        const double d = v.asFloat();
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9.2e18) {
            return Value::integer(static_cast<std::int64_t>(d));
        }
    }
    throw QueryError(ErrorStage::Executor, "cannot store " + std::string(typeName(v.type())) + " value " +
                                               v.toSqlLiteral() + " in " + typeName(target) + " column '" +
                                               columnName + "'");
}

}  // namespace querylab

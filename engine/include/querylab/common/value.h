#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace querylab {

/// SQL column/expression types supported by QueryLab. `Null` is the type of
/// the bare NULL literal before it is coerced into a context.
enum class DataType { Null, Boolean, Integer, Float, Text };

const char* typeName(DataType t);
bool isNumeric(DataType t);

/// A strongly typed SQL value. Integers are 64-bit, floats are IEEE doubles,
/// text is UTF-8 bytes compared bytewise.
class Value {
public:
    using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

    Value() = default;  // NULL
    static Value null() { return Value(); }
    static Value boolean(bool b) { return Value(Storage(b)); }
    static Value integer(std::int64_t i) { return Value(Storage(i)); }
    static Value floating(double d) { return Value(Storage(d)); }
    static Value text(std::string s) { return Value(Storage(std::move(s))); }

    [[nodiscard]] DataType type() const;
    [[nodiscard]] bool isNull() const { return std::holds_alternative<std::monostate>(v_); }
    [[nodiscard]] bool asBool() const { return std::get<bool>(v_); }
    [[nodiscard]] std::int64_t asInt() const { return std::get<std::int64_t>(v_); }
    [[nodiscard]] double asFloat() const { return std::get<double>(v_); }
    [[nodiscard]] const std::string& asText() const { return std::get<std::string>(v_); }
    /// Numeric value as double (Integer or Float). Precondition: numeric.
    [[nodiscard]] double asNumber() const;

    /// Human readable rendering; NULL renders as "NULL", text is unquoted.
    [[nodiscard]] std::string toString() const;
    /// SQL literal rendering; text is single-quoted with '' escaping.
    [[nodiscard]] std::string toSqlLiteral() const;

    /// Total order used by sorting, DISTINCT, GROUP BY and indexes:
    /// NULL < BOOLEAN < numbers (int and float compared numerically) < TEXT.
    /// This is NOT SQL comparison semantics (which yield UNKNOWN for NULL);
    /// see eval.cpp for three-valued comparisons.
    [[nodiscard]] static int compareTotal(const Value& a, const Value& b);
    /// Equality consistent with compareTotal (NULL == NULL here).
    [[nodiscard]] bool operator==(const Value& o) const { return compareTotal(*this, o) == 0; }
    [[nodiscard]] std::size_t hash() const;

    /// Approximate in-memory footprint in bytes, used for memory estimates.
    [[nodiscard]] std::size_t footprint() const;

    [[nodiscard]] const Storage& raw() const { return v_; }

private:
    explicit Value(Storage v) : v_(std::move(v)) {}
    Storage v_;
};

using Row = std::vector<Value>;

struct ValueHash {
    std::size_t operator()(const Value& v) const { return v.hash(); }
};
struct RowHash {
    std::size_t operator()(const Row& r) const;
};
struct RowEq {
    bool operator()(const Row& a, const Row& b) const;
};

int compareRows(const Row& a, const Row& b);

/// Casts `v` to `target` for storage (INSERT/UPDATE). Allows int->float,
/// integral float->int, and NULL to anything. Throws QueryError otherwise.
Value coerceForStorage(const Value& v, DataType target, const std::string& columnName);

}  // namespace querylab

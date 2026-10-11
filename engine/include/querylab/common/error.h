#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>

namespace querylab {

/// A half-open byte range [begin, end) in the SQL source, plus the 1-based
/// line/column of `begin`. Every token and AST node carries one so errors and
/// the AST viewer can point back into the original text.
struct SourceSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    int line = 1;
    int column = 1;

    [[nodiscard]] bool valid() const { return end >= begin; }
    /// Smallest span covering both `a` and `b` (a must start first).
    static SourceSpan cover(const SourceSpan& a, const SourceSpan& b) {
        SourceSpan s = a;
        if (b.end > s.end) s.end = b.end;
        return s;
    }
};

/// The pipeline stage that produced an error. Exposed in API responses so the
/// UI can highlight where the query failed.
enum class ErrorStage { Lexer, Parser, Binder, Planner, Optimizer, Executor, Storage, Catalog, Api };

inline const char* stageName(ErrorStage s) {
    switch (s) {
        case ErrorStage::Lexer: return "lexer";
        case ErrorStage::Parser: return "parser";
        case ErrorStage::Binder: return "binder";
        case ErrorStage::Planner: return "planner";
        case ErrorStage::Optimizer: return "optimizer";
        case ErrorStage::Executor: return "executor";
        case ErrorStage::Storage: return "storage";
        case ErrorStage::Catalog: return "catalog";
        case ErrorStage::Api: return "api";
    }
    return "unknown";
}

/// All user-facing failures (bad SQL, unknown table, type errors, limits) are
/// reported with this exception. Internal invariant violations use asserts or
/// std::logic_error and are treated as bugs.
class QueryError : public std::runtime_error {
public:
    QueryError(ErrorStage stage, std::string message, std::optional<SourceSpan> span = std::nullopt)
        : std::runtime_error(message), stage_(stage), span_(span) {}

    [[nodiscard]] ErrorStage stage() const { return stage_; }
    [[nodiscard]] const std::optional<SourceSpan>& span() const { return span_; }

    /// "parser error at 1:14: expected FROM"
    [[nodiscard]] std::string describe() const {
        std::string out = std::string(stageName(stage_)) + " error";
        if (span_) out += " at " + std::to_string(span_->line) + ":" + std::to_string(span_->column);
        out += ": ";
        out += what();
        return out;
    }

private:
    ErrorStage stage_;
    std::optional<SourceSpan> span_;
};

}  // namespace querylab

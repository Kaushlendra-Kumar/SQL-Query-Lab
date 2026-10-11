#include "querylab/execution/executor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "querylab/storage/page.h"

namespace querylab::execution {

using planner::PhysicalNode;
using planner::PhysicalOp;
using storage::Rid;

// ---- context -------------------------------------------------------------------

ExecContext::ExecContext(catalog::Catalog& cat, ExecLimits l, const std::atomic<bool>* cancel)
    : catalog(cat), limits(l), start_(std::chrono::steady_clock::now()), deadline_(start_ + l.timeout), cancel_(cancel) {}

void ExecContext::check() {
    if (cancel_ && cancel_->load(std::memory_order_relaxed)) {
        throw QueryError(ErrorStage::Executor, "query cancelled by request");
    }
    if (std::chrono::steady_clock::now() > deadline_) {
        throw QueryError(ErrorStage::Executor, "query exceeded the time limit of " + std::to_string(limits.timeout.count()) +
                                                   " ms and was stopped (raise it in settings, or add a LIMIT / more selective filter)");
    }
}

std::int64_t ExecContext::nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count();
}

void ExecContext::event(int nodeId, const char* type, std::uint64_t rows) {
    if (!limits.recordEvents) return;
    const bool isRow = type[0] == 'r' && type[1] == 'o';
    if (isRow && events.size() >= limits.maxEvents) {
        eventsTruncated = true;
        return;
    }
    events.push_back(ExecEvent{events.size(), nodeId, type, rows, nowNs()});
}

void ExecContext::addMemory(std::uint64_t bytes, OperatorStats& st) {
    memoryBytes += bytes;
    st.peakMemoryBytes += bytes;
    if (memoryBytes > limits.maxMemoryBytes) {
        throw QueryError(ErrorStage::Executor, "query exceeded the memory limit of " + std::to_string(limits.maxMemoryBytes / (1024 * 1024)) +
                                                   " MB while buffering rows");
    }
}

double timerOverheadNs() {
    using clock = std::chrono::steady_clock;
    std::vector<double> samples;
    for (int s = 0; s < 9; ++s) {
        constexpr int kCalls = 2000;
        const auto t0 = clock::now();
        clock::time_point sink{};
        for (int i = 0; i < kCalls; ++i) sink = std::max(sink, clock::now());
        const auto t1 = clock::now();
        (void)sink;
        samples.push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / kCalls);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

// ---- operator base ------------------------------------------------------------------

namespace {

std::size_t rowBytes(const Row& r) {
    std::size_t b = sizeof(Row);
    for (const auto& v : r) b += v.footprint();
    return b;
}

class Operator {
public:
    Operator(const PhysicalNode& node, ExecContext& ctx) : node_(node), ctx_(ctx), st_(ctx.stats[node.id]) {
        st_.nodeId = node.id;
        st_.name = planner::physicalOpName(node.op);
    }
    virtual ~Operator() = default;
    Operator(const Operator&) = delete;
    Operator& operator=(const Operator&) = delete;

    void open() {
        const auto t0 = startTimer();
        ctx_.event(node_.id, "open", 0);
        st_.opened = true;
        doOpen();
        stopTimer(t0);
    }

    bool next(Row& out) {
        const auto t0 = startTimer();
        ++st_.nextCalls;
        if ((st_.nextCalls & 255) == 0) ctx_.check();
        const bool ok = !st_.exhausted && doNext(out);
        if (ok) {
            ++st_.rowsOut;
            ctx_.event(node_.id, "row", st_.rowsOut);
        } else if (!st_.exhausted) {
            st_.exhausted = true;
            ctx_.event(node_.id, "done", st_.rowsOut);
        }
        stopTimer(t0);
        return ok;
    }

    void close() {
        const auto t0 = startTimer();
        doClose();
        for (auto& c : children_) c->close();
        ctx_.event(node_.id, "close", st_.rowsOut);
        stopTimer(t0);
    }

    /// RID of the row most recently returned (scans only).
    virtual Rid currentRid() const { throw std::logic_error("operator does not expose row ids"); }

    std::vector<std::unique_ptr<Operator>> children_;

protected:
    virtual void doOpen() {
        for (auto& c : children_) c->open();
    }
    virtual bool doNext(Row& out) = 0;
    virtual void doClose() {}

    std::int64_t startTimer() {
        if (!ctx_.limits.timing) return 0;
        ++ctx_.timerCalls;
        return ctx_.nowNs();
    }
    void stopTimer(std::int64_t t0) {
        if (!ctx_.limits.timing) return;
        ++ctx_.timerCalls;
        st_.inclusiveNs += ctx_.nowNs() - t0;
    }

    /// Fetches a page, accounting page reads / misses to this operator.
    storage::PageGuard fetchPage(storage::PageId id) {
        auto& pool = ctx_.catalog.pool();
        const auto missesBefore = pool.stats().misses;
        auto g = pool.fetch(id);
        ++st_.pagesRead;
        st_.pageMisses += pool.stats().misses - missesBefore;
        return g;
    }

    const PhysicalNode& node_;
    ExecContext& ctx_;
    OperatorStats& st_;
};

// ---- scans ------------------------------------------------------------------------

/// Shared scan logic: predicate-first decoding (late materialization).
class ScanBase : public Operator {
public:
    ScanBase(const PhysicalNode& n, ExecContext& c) : Operator(n, c) {
        const auto width = n.table->columns.size();
        wantPred_.assign(width, false);
        wantRest_.assign(width, false);
        if (n.predicate) {
            std::set<planner::ColumnId> ids;
            n.predicate->collectColumns(ids);
            const auto layout = planner::tableLayout(n);
            for (std::size_t i = 0; i < layout.size(); ++i) {
                if (layout[i].id >= 0 && ids.count(layout[i].id)) wantPred_[i] = true;
            }
        }
        for (auto c2 : n.scanColumns) {
            if (!wantPred_[c2]) wantRest_[c2] = true;
        }
    }

protected:
    /// Decodes a row image; returns false if the scan predicate rejects it.
    bool materialize(std::span<const std::uint8_t> bytes, Row& out) {
        storage::codec::decodeColumns(bytes, wantPred_, scratch_, true);
        if (node_.predicate && !planner::isTrue(planner::evaluate(*node_.predicate, scratch_))) {
            ++st_.rowsFiltered;
            return false;
        }
        storage::codec::decodeColumns(bytes, wantRest_, scratch_, false);
        out.resize(node_.scanColumns.size());
        for (std::size_t i = 0; i < node_.scanColumns.size(); ++i) out[i] = std::move(scratch_[node_.scanColumns[i]]);
        return true;
    }

    std::vector<bool> wantPred_, wantRest_;
    Row scratch_;
};

class SeqScanOp final : public ScanBase {
public:
    using ScanBase::ScanBase;
    Rid currentRid() const override { return batch_[pos_ - 1].first; }

protected:
    void doOpen() override {
        pageIdx_ = 0;
        batch_.clear();
        pos_ = 0;
    }
    bool doNext(Row& out) override {
        while (pos_ >= batch_.size()) {
            const auto& pages = node_.table->heap->pages();
            if (pageIdx_ >= pages.size()) return false;
            ctx_.check();
            batch_.clear();
            pos_ = 0;
            const auto pid = pages[pageIdx_++];
            auto g = fetchPage(pid);
            const auto& page = g.page();
            for (std::uint16_t s = 0; s < page.slotCount(); ++s) {
                if (!page.isLive(s)) continue;
                ++st_.rowsScanned;
                Row r;
                if (materialize(page.get(s), r)) batch_.emplace_back(Rid{pid, s}, std::move(r));
            }
        }
        out = std::move(batch_[pos_++].second);
        return true;
    }

private:
    std::size_t pageIdx_ = 0;
    std::vector<std::pair<Rid, Row>> batch_;
    std::size_t pos_ = 0;
};

class IndexScanOp final : public ScanBase {
public:
    using ScanBase::ScanBase;
    Rid currentRid() const override { return rid_; }

protected:
    void doOpen() override {
        const auto& b = node_.bounds;
        Row low = b.eqPrefix;
        bool inclusive = true;
        if (b.lo) {
            low.push_back(*b.lo);
            inclusive = b.loInclusive;
        }
        index::IndexScanStats s;
        cursor_ = b.index->tree->seek(low, inclusive, s);
        absorb(s);
    }

    bool doNext(Row& out) override {
        const auto& b = node_.bounds;
        const std::size_t k = b.eqPrefix.size();
        while (cursor_.valid()) {
            const auto& e = cursor_.entry();
            if (k > 0 && index::BPlusTree::comparePrefix(e.key, b.eqPrefix) != 0) return finish();
            bool skip = false;
            if ((b.lo || b.hi) && k < e.key.size()) {
                const Value& v = e.key[k];
                if (v.isNull()) {
                    skip = true;  // NULL never satisfies a range comparison
                } else if (b.hi) {
                    const int c = Value::compareTotal(v, *b.hi);
                    if (c > 0 || (c == 0 && !b.hiInclusive)) return finish();
                }
            }
            const Rid rid = e.rid;
            advance();
            if (skip) continue;
            ++st_.rowsScanned;
            auto g = fetchPage(rid.page);
            if (materialize(g.page().get(rid.slot), out)) {
                rid_ = rid;
                return true;
            }
        }
        return finish();
    }

private:
    void advance() {
        index::IndexScanStats s;
        cursor_.next(s);
        absorb(s);
    }
    void absorb(const index::IndexScanStats& s) {
        st_.indexNodesVisited += s.nodesVisited;
        st_.indexEntriesExamined += s.entriesExamined;
    }
    bool finish() {
        cursor_ = index::BPlusTree::Cursor{};
        return false;
    }

    index::BPlusTree::Cursor cursor_;
    Rid rid_;
};

// ---- simple operators -------------------------------------------------------------

class FilterOp final : public Operator {
public:
    using Operator::Operator;

protected:
    bool doNext(Row& out) override {
        while (children_[0]->next(out)) {
            if (planner::isTrue(planner::evaluate(*node_.predicate, out))) return true;
            ++st_.rowsFiltered;
        }
        return false;
    }
};

class ProjectOp final : public Operator {
public:
    using Operator::Operator;

protected:
    bool doNext(Row& out) override {
        if (!children_[0]->next(in_)) return false;
        out.resize(node_.exprs.size());
        for (std::size_t i = 0; i < node_.exprs.size(); ++i) out[i] = planner::evaluate(*node_.exprs[i], in_);
        return true;
    }

private:
    Row in_;
};

class LimitOp final : public Operator {
public:
    using Operator::Operator;

protected:
    bool doNext(Row& out) override {
        while (skipped_ < node_.offset) {
            if (!children_[0]->next(out)) return false;
            ++skipped_;
        }
        if (node_.limit && emitted_ >= *node_.limit) return false;  // stop pulling: early termination
        if (!children_[0]->next(out)) return false;
        ++emitted_;
        return true;
    }

private:
    std::int64_t skipped_ = 0, emitted_ = 0;
};

class ValuesOp final : public Operator {
public:
    using Operator::Operator;

protected:
    bool doNext(Row& out) override {
        if (done_) return false;
        done_ = true;
        out.clear();
        return true;
    }

private:
    bool done_ = false;
};

class EmptyOp final : public Operator {
public:
    using Operator::Operator;

protected:
    bool doNext(Row&) override { return false; }
};

// ---- joins --------------------------------------------------------------------------

/// Writes l ++ r into `out`, reusing its capacity (no allocation once warm).
void concatInto(Row& out, const Row& l, const Row& r) {
    out.clear();
    out.insert(out.end(), l.begin(), l.end());
    out.insert(out.end(), r.begin(), r.end());
}

Row concat(const Row& l, const Row& r) {
    Row out;
    out.reserve(l.size() + r.size());
    out.insert(out.end(), l.begin(), l.end());
    out.insert(out.end(), r.begin(), r.end());
    return out;
}

class NestedLoopJoinOp final : public Operator {
public:
    using Operator::Operator;

protected:
    void doOpen() override {
        Operator::doOpen();
        Row r;
        while (children_[1]->next(r)) {
            ctx_.addMemory(rowBytes(r), st_);
            inner_.push_back(r);
        }
        st_.buildRows = inner_.size();
        rightWidth_ = node_.children[1]->output.size();
        ctx_.event(node_.id, "build_done", inner_.size());
    }

    bool doNext(Row& out) override {
        while (true) {
            if (!haveOuter_) {
                if (!children_[0]->next(outer_)) return false;
                haveOuter_ = true;
                matched_ = false;
                pos_ = 0;
            }
            while (pos_ < inner_.size()) {
                if ((++comparisons_ & 4095) == 0) ctx_.check();
                concatInto(out, outer_, inner_[pos_++]);
                if (!node_.predicate || planner::isTrue(planner::evaluate(*node_.predicate, out))) {
                    matched_ = true;
                    return true;
                }
                ++st_.rowsFiltered;
            }
            haveOuter_ = false;
            if (node_.joinType == sql::JoinType::Left && !matched_) {
                out = concat(outer_, Row(rightWidth_));
                return true;
            }
        }
    }

private:
    std::vector<Row> inner_;
    Row outer_;
    bool haveOuter_ = false, matched_ = false;
    std::size_t pos_ = 0, rightWidth_ = 0;
    std::uint64_t comparisons_ = 0;
};

class HashJoinOp final : public Operator {
public:
    using Operator::Operator;

protected:
    void doOpen() override {
        Operator::doOpen();
        Row r;
        while (children_[1]->next(r)) {
            ++st_.buildRows;
            Row key = keyOf(node_.rightKeys, r);
            if (hasNull(key)) continue;  // NULL keys never match
            ctx_.addMemory(rowBytes(r) + rowBytes(key), st_);
            table_[std::move(key)].push_back(r);
        }
        st_.groups = table_.size();
        rightWidth_ = node_.children[1]->output.size();
        ctx_.event(node_.id, "build_done", st_.buildRows);
    }

    bool doNext(Row& out) override {
        while (true) {
            if (!haveProbe_) {
                if (!children_[0]->next(probe_)) return false;
                haveProbe_ = true;
                matched_ = false;
                bucket_ = nullptr;
                pos_ = 0;
                keyInto(node_.leftKeys, probe_, probeKey_);
                if (!hasNull(probeKey_)) {
                    auto it = table_.find(probeKey_);
                    if (it != table_.end()) bucket_ = &it->second;
                }
            }
            while (bucket_ && pos_ < bucket_->size()) {
                concatInto(out, probe_, (*bucket_)[pos_++]);
                if (!node_.predicate || planner::isTrue(planner::evaluate(*node_.predicate, out))) {
                    matched_ = true;
                    return true;
                }
                ++st_.rowsFiltered;
            }
            haveProbe_ = false;
            if (node_.joinType == sql::JoinType::Left && !matched_) {
                out = concat(probe_, Row(rightWidth_));
                return true;
            }
        }
    }

private:
    static void keyInto(const std::vector<planner::BoundExprPtr>& keys, const Row& r, Row& k) {
        k.clear();
        for (const auto& e : keys) k.push_back(planner::evaluate(*e, r));
    }
    static Row keyOf(const std::vector<planner::BoundExprPtr>& keys, const Row& r) {
        Row k;
        k.reserve(keys.size());
        for (const auto& e : keys) k.push_back(planner::evaluate(*e, r));
        return k;
    }
    static bool hasNull(const Row& k) {
        return std::any_of(k.begin(), k.end(), [](const Value& v) { return v.isNull(); });
    }

    std::unordered_map<Row, std::vector<Row>, RowHash, RowEq> table_;
    Row probe_, probeKey_;
    bool haveProbe_ = false, matched_ = false;
    const std::vector<Row>* bucket_ = nullptr;
    std::size_t pos_ = 0, rightWidth_ = 0;
};

// ---- sort / aggregate / distinct ------------------------------------------------------

class SortOp final : public Operator {
public:
    using Operator::Operator;

protected:
    void doOpen() override {
        Operator::doOpen();
        std::vector<std::pair<Row, Row>> items;
        Row r;
        while (children_[0]->next(r)) {
            Row keys;
            for (const auto& k : node_.sortKeys) keys.push_back(planner::evaluate(*k.expr, r));
            ctx_.addMemory(rowBytes(r) + rowBytes(keys), st_);
            items.emplace_back(std::move(keys), std::move(r));
        }
        st_.buildRows = items.size();
        // NULL sorts as the smallest value (NULLS FIRST for ASC, LAST for DESC), as in SQLite.
        std::stable_sort(items.begin(), items.end(), [&](const auto& a, const auto& b) {
            for (std::size_t i = 0; i < node_.sortKeys.size(); ++i) {
                int c = Value::compareTotal(a.first[i], b.first[i]);
                if (node_.sortKeys[i].descending) c = -c;
                if (c != 0) return c < 0;
            }
            return false;
        });
        rows_.reserve(items.size());
        for (auto& it : items) rows_.push_back(std::move(it.second));
        ctx_.event(node_.id, "sort_done", rows_.size());
    }
    bool doNext(Row& out) override {
        if (pos_ >= rows_.size()) return false;
        out = std::move(rows_[pos_++]);
        return true;
    }

private:
    std::vector<Row> rows_;
    std::size_t pos_ = 0;
};

struct AggState {
    std::uint64_t count = 0;
    std::int64_t isum = 0;
    double fsum = 0;
    bool sawFloat = false;
    bool any = false;
    Value best;  // min / max
    std::unordered_set<Value, ValueHash> seen;  // DISTINCT
};

class HashAggregateOp final : public Operator {
public:
    using Operator::Operator;

protected:
    void doOpen() override {
        Operator::doOpen();
        const auto& aggs = node_.aggregates;
        Row r, key;
        while (children_[0]->next(r)) {
            ++st_.buildRows;
            key.clear();
            for (const auto& g : node_.groupBy) key.push_back(planner::evaluate(*g, r));
            auto it = index_.find(key);
            if (it == index_.end()) {
                ctx_.addMemory(rowBytes(key) + aggs.size() * sizeof(AggState), st_);
                it = index_.emplace(key, groups_.size()).first;
                groups_.push_back({key, std::vector<AggState>(aggs.size())});
            }
            auto& states = groups_[it->second].second;
            for (std::size_t i = 0; i < aggs.size(); ++i) accumulate(aggs[i], states[i], r);
        }
        if (groups_.empty() && node_.groupBy.empty()) {
            groups_.push_back({Row{}, std::vector<AggState>(aggs.size())});  // global aggregate over no rows
        }
        st_.groups = groups_.size();
        ctx_.event(node_.id, "aggregate_done", groups_.size());
    }

    bool doNext(Row& out) override {
        if (pos_ >= groups_.size()) return false;
        auto& [key, states] = groups_[pos_++];
        out = key;
        for (std::size_t i = 0; i < node_.aggregates.size(); ++i) out.push_back(finalize(node_.aggregates[i], states[i]));
        return true;
    }

private:
    void accumulate(const planner::AggregateCall& a, AggState& s, const Row& r) {
        if (!a.arg) {  // COUNT(*)
            ++s.count;
            return;
        }
        Value v = planner::evaluate(*a.arg, r);
        if (v.isNull()) return;
        if (a.distinct) {
            if (!s.seen.insert(v).second) return;
            ctx_.addMemory(v.footprint() + 16, st_);
        }
        ++s.count;
        if (a.func == "sum" || a.func == "avg") {
            if (v.type() == DataType::Float) {
                if (!s.sawFloat) {
                    s.fsum = static_cast<double>(s.isum);
                    s.sawFloat = true;
                }
                s.fsum += v.asFloat();
            } else if (s.sawFloat) {
                s.fsum += static_cast<double>(v.asInt());
            } else {
                std::int64_t res;
                bool overflow;
#if defined(__GNUC__) || defined(__clang__)
                overflow = __builtin_add_overflow(s.isum, v.asInt(), &res);
#else
                overflow = (v.asInt() > 0 && s.isum > INT64_MAX - v.asInt()) || (v.asInt() < 0 && s.isum < INT64_MIN - v.asInt());
                res = overflow ? 0 : s.isum + v.asInt();
#endif
                if (overflow) {
                    if (a.func == "sum") throw QueryError(ErrorStage::Executor, "integer overflow in SUM");
                    s.fsum = static_cast<double>(s.isum) + static_cast<double>(v.asInt());
                    s.sawFloat = true;
                } else {
                    s.isum = res;
                }
            }
        } else if (a.func == "min" || a.func == "max") {
            if (!s.any) {
                s.best = v;
                s.any = true;
            } else {
                const int c = Value::compareTotal(v, s.best);
                if ((a.func == "min" && c < 0) || (a.func == "max" && c > 0)) s.best = v;
            }
        }
    }

    static Value finalize(const planner::AggregateCall& a, const AggState& s) {
        if (a.func == "count") return Value::integer(static_cast<std::int64_t>(s.count));
        if (s.count == 0) return Value::null();
        if (a.func == "sum") return s.sawFloat ? Value::floating(s.fsum) : Value::integer(s.isum);
        if (a.func == "avg") {
            const double total = s.sawFloat ? s.fsum : static_cast<double>(s.isum);
            return Value::floating(total / static_cast<double>(s.count));
        }
        return s.best;
    }

    std::unordered_map<Row, std::size_t, RowHash, RowEq> index_;
    std::vector<std::pair<Row, std::vector<AggState>>> groups_;
    std::size_t pos_ = 0;
};

class HashDistinctOp final : public Operator {
public:
    using Operator::Operator;

protected:
    bool doNext(Row& out) override {
        while (children_[0]->next(out)) {
            if (seen_.insert(out).second) {
                ctx_.addMemory(rowBytes(out), st_);
                st_.groups = seen_.size();
                return true;
            }
            ++st_.rowsFiltered;
        }
        return false;
    }

private:
    std::unordered_set<Row, RowHash, RowEq> seen_;
};

// ---- DML -----------------------------------------------------------------------------

class ModifyOp final : public Operator {
public:
    ModifyOp(const PhysicalNode& n, ExecContext& c, std::uint64_t& affected) : Operator(n, c), affected_(affected) {}

protected:
    void doOpen() override {
        Operator::doOpen();
        // Collect targets first so modifications cannot affect the scan
        // (avoids the "Halloween problem").
        std::vector<std::pair<Rid, Row>> targets;
        Row r;
        Operator* scan = children_[0].get();
        while (scan->next(r)) targets.emplace_back(scan->currentRid(), r);
        auto& table = *ctx_.catalog.findTable(node_.table->name);
        // Statement-level atomicity: if any row fails (unique violation,
        // overflow, ...), undo the changes this statement already made.
        struct Undo {
            Rid rid;      // current location of the changed row
            Row before;   // original image
            Row after;    // new image (UPDATE only)
        };
        std::vector<Undo> undo;
        try {
            if (node_.op == PhysicalOp::Delete) {
                for (const auto& [rid, row] : targets) {
                    ctx_.catalog.deleteRow(table, rid, row);
                    undo.push_back({rid, row, {}});
                }
            } else {
                for (const auto& [rid, row] : targets) {
                    Row updated = row;
                    for (const auto& [col, expr] : node_.assignments) {
                        updated[col] = coerceForStorage(planner::evaluate(*expr, row), table.columns[col].type, table.columns[col].name);
                    }
                    const Rid newRid = ctx_.catalog.updateRow(table, rid, row, updated);
                    undo.push_back({newRid, row, std::move(updated)});
                }
            }
        } catch (...) {
            for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
                if (node_.op == PhysicalOp::Delete) ctx_.catalog.insertRow(table, it->before);
                else ctx_.catalog.updateRow(table, it->rid, it->after, it->before);
            }
            throw;
        }
        affected_ = targets.size();
        st_.buildRows = targets.size();
    }
    bool doNext(Row&) override { return false; }

private:
    std::uint64_t& affected_;
};

std::unique_ptr<Operator> buildOperator(const PhysicalNode& n, ExecContext& ctx, std::uint64_t& affected) {
    std::unique_ptr<Operator> op;
    switch (n.op) {
        case PhysicalOp::SeqScan: op = std::make_unique<SeqScanOp>(n, ctx); break;
        case PhysicalOp::IndexScan: op = std::make_unique<IndexScanOp>(n, ctx); break;
        case PhysicalOp::Filter: op = std::make_unique<FilterOp>(n, ctx); break;
        case PhysicalOp::Project: op = std::make_unique<ProjectOp>(n, ctx); break;
        case PhysicalOp::NestedLoopJoin: op = std::make_unique<NestedLoopJoinOp>(n, ctx); break;
        case PhysicalOp::HashJoin: op = std::make_unique<HashJoinOp>(n, ctx); break;
        case PhysicalOp::Sort: op = std::make_unique<SortOp>(n, ctx); break;
        case PhysicalOp::Limit: op = std::make_unique<LimitOp>(n, ctx); break;
        case PhysicalOp::HashAggregate: op = std::make_unique<HashAggregateOp>(n, ctx); break;
        case PhysicalOp::HashDistinct: op = std::make_unique<HashDistinctOp>(n, ctx); break;
        case PhysicalOp::Values: op = std::make_unique<ValuesOp>(n, ctx); break;
        case PhysicalOp::Empty: op = std::make_unique<EmptyOp>(n, ctx); break;
        case PhysicalOp::Update:
        case PhysicalOp::Delete: op = std::make_unique<ModifyOp>(n, ctx, affected); break;
    }
    for (const auto& c : n.children) op->children_.push_back(buildOperator(*c, ctx, affected));
    return op;
}

void fillRowsIn(const PhysicalNode& n, ExecContext& ctx) {
    auto& st = ctx.stats[n.id];
    if (n.op == PhysicalOp::SeqScan || n.op == PhysicalOp::IndexScan) {
        st.rowsIn = st.rowsScanned;
    } else {
        std::uint64_t in = 0;
        for (const auto& c : n.children) in += ctx.stats[c->id].rowsOut;
        st.rowsIn = in;
    }
    for (const auto& c : n.children) fillRowsIn(*c, ctx);
}

}  // namespace

ExecutionResult execute(const PhysicalNode& plan, ExecContext& ctx) {
    ExecutionResult res;
    const auto t0 = std::chrono::steady_clock::now();
    auto root = buildOperator(plan, ctx, res.affectedRows);
    root->open();
    Row r;
    while (root->next(r)) {
        ++res.totalRows;
        if (res.rows.size() < ctx.limits.maxResultRows) res.rows.push_back(r);
        else res.truncated = true;
    }
    root->close();
    res.executeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    fillRowsIn(plan, ctx);
    return res;
}

}  // namespace querylab::execution

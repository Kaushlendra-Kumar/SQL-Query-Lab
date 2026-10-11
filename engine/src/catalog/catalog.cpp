#include "querylab/catalog/catalog.h"

#include <algorithm>
#include <set>

#include "querylab/common/error.h"

namespace querylab::catalog {

// ---- ColumnStats -----------------------------------------------------------

void ColumnStats::add(const Value& v) {
    if (v.isNull()) {
        ++nulls_;
        return;
    }
    ++counts_[v];
    ++nonNull_;
    histogramStale_ = true;
}

void ColumnStats::remove(const Value& v) {
    if (v.isNull()) {
        if (nulls_) --nulls_;
        return;
    }
    auto it = counts_.find(v);
    if (it == counts_.end()) return;
    if (--it->second == 0) counts_.erase(it);
    --nonNull_;
    histogramStale_ = true;
}

std::optional<Value> ColumnStats::min() const {
    if (counts_.empty()) return std::nullopt;
    return counts_.begin()->first;
}

std::optional<Value> ColumnStats::max() const {
    if (counts_.empty()) return std::nullopt;
    return counts_.rbegin()->first;
}

const std::vector<ColumnStats::Bucket>& ColumnStats::histogram() const {
    if (!histogramStale_) return histogram_;
    histogram_.clear();
    histogramStale_ = false;
    if (nonNull_ == 0) return histogram_;
    const double target = static_cast<double>(nonNull_) / static_cast<double>(kHistogramBuckets);
    Bucket cur;
    bool open = false;
    for (const auto& [v, c] : counts_) {
        if (!open) {
            cur = Bucket{v, v, 0};
            open = true;
        }
        cur.high = v;
        cur.count += c;
        if (static_cast<double>(cur.count) >= target) {
            histogram_.push_back(cur);
            open = false;
        }
    }
    if (open) histogram_.push_back(cur);
    return histogram_;
}

double ColumnStats::fractionBelow(const Value& v, bool inclusive) const {
    const auto& h = histogram();
    if (h.empty() || nonNull_ == 0) return 0.0;
    const double total = static_cast<double>(nonNull_);
    double below = 0;
    for (const auto& b : h) {
        const int cLow = Value::compareTotal(v, b.low);
        const int cHigh = Value::compareTotal(v, b.high);
        if (cLow < 0 || (cLow == 0 && !inclusive)) return below / total;
        if (cHigh > 0 || (cHigh == 0 && inclusive)) {
            below += static_cast<double>(b.count);
            continue;
        }
        // v falls strictly inside the bucket: interpolate numbers, else half.
        double frac = 0.5;
        if (isNumeric(v.type()) && isNumeric(b.low.type()) && isNumeric(b.high.type())) {
            const double lo = b.low.asNumber(), hi = b.high.asNumber();
            if (hi > lo) frac = std::clamp((v.asNumber() - lo) / (hi - lo), 0.0, 1.0);
        }
        below += frac * static_cast<double>(b.count);
        return below / total;
    }
    return below / total;
}

// ---- IndexInfo / TableInfo ---------------------------------------------------

index::Key IndexInfo::keyFor(const Row& row) const {
    index::Key k;
    k.reserve(columns.size());
    for (auto c : columns) k.push_back(row[c]);
    return k;
}

std::string IndexInfo::definition(const TableInfo& t) const {
    std::string s = unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ";
    s += name + " ON " + t.name + " (";
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i) s += ", ";
        s += t.columns[columns[i]].name;
    }
    return s + ")";
}

std::optional<std::size_t> TableInfo::columnIndex(const std::string& col) const {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].name == col) return i;
    }
    return std::nullopt;
}

// ---- Catalog -----------------------------------------------------------------

TableInfo& Catalog::createTable(const std::string& name, std::vector<ColumnInfo> columns) {
    if (tables_.count(name)) throw QueryError(ErrorStage::Catalog, "table '" + name + "' already exists");
    if (columns.empty()) throw QueryError(ErrorStage::Catalog, "a table needs at least one column");
    std::set<std::string> seen;
    for (const auto& c : columns) {
        if (!seen.insert(c.name).second) throw QueryError(ErrorStage::Catalog, "duplicate column '" + c.name + "'");
    }
    return restoreTable(nextTableId_, name, std::move(columns), {}, 0);
}

TableInfo& Catalog::restoreTable(std::uint32_t id, const std::string& name, std::vector<ColumnInfo> columns,
                                 std::vector<storage::PageId> pages, std::size_t rowCount) {
    auto t = std::make_unique<TableInfo>();
    t->id = id;
    t->name = name;
    t->columns = std::move(columns);
    t->heap = std::make_unique<storage::TableHeap>(*pool_, t->columns.size());
    t->heap->restore(std::move(pages), rowCount);
    t->stats.resize(t->columns.size());
    nextTableId_ = std::max(nextTableId_, id + 1);
    auto& ref = *t;
    tables_[name] = std::move(t);
    ++schemaVersion_;
    return ref;
}

void Catalog::rebuildStats(TableInfo& t) {
    t.stats.assign(t.columns.size(), ColumnStats{});
    t.heap->forEach([&](const storage::Rid&, const Row& row) {
        for (std::size_t i = 0; i < row.size(); ++i) t.stats[i].add(row[i]);
    });
}

void Catalog::dropTable(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) throw QueryError(ErrorStage::Catalog, "table '" + name + "' does not exist");
    for (auto* idx : it->second->indexes) indexes_.erase(idx->name);
    // Pages are abandoned (no free-space map); documented limitation.
    tables_.erase(it);
    ++schemaVersion_;
}

IndexInfo& Catalog::createIndex(const std::string& name, const std::string& table, const std::vector<std::string>& columns,
                                bool unique) {
    if (indexes_.count(name)) throw QueryError(ErrorStage::Catalog, "index '" + name + "' already exists");
    TableInfo* t = findTable(table);
    if (!t) throw QueryError(ErrorStage::Catalog, "table '" + table + "' does not exist");
    auto idx = std::make_unique<IndexInfo>();
    idx->name = name;
    idx->table = table;
    idx->unique = unique;
    std::set<std::size_t> seen;
    for (const auto& c : columns) {
        auto pos = t->columnIndex(c);
        if (!pos) throw QueryError(ErrorStage::Catalog, "column '" + c + "' does not exist in table '" + table + "'");
        if (!seen.insert(*pos).second) throw QueryError(ErrorStage::Catalog, "column '" + c + "' listed twice in index");
        idx->columns.push_back(*pos);
    }
    idx->tree = std::make_unique<index::BPlusTree>(idx->columns.size());
    t->heap->forEach([&](const storage::Rid& rid, const Row& row) {
        auto key = idx->keyFor(row);
        const bool hasNull = std::any_of(key.begin(), key.end(), [](const Value& v) { return v.isNull(); });
        if (unique && !hasNull && idx->tree->containsKey(key)) {
            throw QueryError(ErrorStage::Catalog, "cannot create unique index '" + name + "': duplicate key " + key[0].toSqlLiteral() +
                                                      (key.size() > 1 ? ", ..." : ""));
        }
        idx->tree->insert(key, rid);
    });
    auto& ref = *idx;
    t->indexes.push_back(idx.get());
    indexes_[name] = std::move(idx);
    ++schemaVersion_;
    return ref;
}

void Catalog::dropIndex(const std::string& name) {
    auto it = indexes_.find(name);
    if (it == indexes_.end()) throw QueryError(ErrorStage::Catalog, "index '" + name + "' does not exist");
    if (TableInfo* t = findTable(it->second->table)) {
        std::erase(t->indexes, it->second.get());
    }
    indexes_.erase(it);
    ++schemaVersion_;
}

TableInfo* Catalog::findTable(const std::string& name) const {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : it->second.get();
}

IndexInfo* Catalog::findIndex(const std::string& name) const {
    auto it = indexes_.find(name);
    return it == indexes_.end() ? nullptr : it->second.get();
}

std::vector<TableInfo*> Catalog::tables() const {
    std::vector<TableInfo*> out;
    for (const auto& [_, t] : tables_) out.push_back(t.get());
    return out;
}

std::vector<IndexInfo*> Catalog::indexes() const {
    std::vector<IndexInfo*> out;
    for (const auto& [_, i] : indexes_) out.push_back(i.get());
    return out;
}

void Catalog::checkUnique(const TableInfo& t, const Row& row, const storage::Rid* ignore) const {
    for (const auto* idx : t.indexes) {
        if (!idx->unique) continue;
        auto key = idx->keyFor(row);
        if (std::any_of(key.begin(), key.end(), [](const Value& v) { return v.isNull(); })) continue;
        index::IndexScanStats s;
        auto c = idx->tree->seek(key, true, s);
        while (c.valid() && compareRows(c.entry().key, key) == 0) {
            if (!ignore || !(c.entry().rid == *ignore)) {
                throw QueryError(ErrorStage::Executor, "duplicate key violates unique index '" + idx->name + "'");
            }
            c.next(s);
        }
    }
}

storage::Rid Catalog::insertRow(TableInfo& t, const Row& row) {
    checkUnique(t, row, nullptr);
    const auto rid = t.heap->insert(row);
    for (auto* idx : t.indexes) idx->tree->insert(idx->keyFor(row), rid);
    for (std::size_t i = 0; i < row.size(); ++i) t.stats[i].add(row[i]);
    ++t.version;
    return rid;
}

void Catalog::deleteRow(TableInfo& t, const storage::Rid& rid, const Row& row) {
    t.heap->erase(rid);
    for (auto* idx : t.indexes) idx->tree->erase(idx->keyFor(row), rid);
    for (std::size_t i = 0; i < row.size(); ++i) t.stats[i].remove(row[i]);
    ++t.version;
}

storage::Rid Catalog::updateRow(TableInfo& t, const storage::Rid& rid, const Row& oldRow, const Row& newRow) {
    checkUnique(t, newRow, &rid);
    const auto newRid = t.heap->update(rid, newRow);
    for (auto* idx : t.indexes) {
        idx->tree->erase(idx->keyFor(oldRow), rid);
        idx->tree->insert(idx->keyFor(newRow), newRid);
    }
    for (std::size_t i = 0; i < oldRow.size(); ++i) {
        t.stats[i].remove(oldRow[i]);
        t.stats[i].add(newRow[i]);
    }
    ++t.version;
    return newRid;
}

}  // namespace querylab::catalog

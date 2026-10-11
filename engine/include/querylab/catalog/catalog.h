#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "querylab/common/value.h"
#include "querylab/index/bplus_tree.h"
#include "querylab/storage/table_heap.h"

namespace querylab::catalog {

struct ColumnInfo {
    std::string name;
    DataType type = DataType::Integer;
};

struct ValueLess {
    bool operator()(const Value& a, const Value& b) const { return Value::compareTotal(a, b) < 0; }
};

/// Per-column statistics maintained incrementally on every insert/delete.
///
/// Exact value frequencies are kept, but the optimizer only sees the summary
/// a real system would have: distinct count, null count, min/max and an
/// equi-depth histogram. Equality estimates therefore assume a uniform
/// distribution (1 / distinct) — deliberately, so estimate-vs-actual
/// differences are meaningful.
class ColumnStats {
public:
    static constexpr std::size_t kHistogramBuckets = 32;

    void add(const Value& v);
    void remove(const Value& v);

    [[nodiscard]] std::size_t distinct() const { return counts_.size(); }
    [[nodiscard]] std::uint64_t nullCount() const { return nulls_; }
    [[nodiscard]] std::uint64_t nonNullCount() const { return nonNull_; }
    [[nodiscard]] std::optional<Value> min() const;
    [[nodiscard]] std::optional<Value> max() const;

    /// Estimated fraction of non-null values strictly below `v` (or <= when
    /// `inclusive`), from the histogram with linear interpolation for numbers.
    [[nodiscard]] double fractionBelow(const Value& v, bool inclusive) const;

    struct Bucket {
        Value low, high;
        std::uint64_t count = 0;
    };
    [[nodiscard]] const std::vector<Bucket>& histogram() const;

private:
    std::map<Value, std::uint32_t, ValueLess> counts_;
    std::uint64_t nulls_ = 0;
    std::uint64_t nonNull_ = 0;
    mutable std::vector<Bucket> histogram_;
    mutable bool histogramStale_ = true;
};

struct TableInfo;

struct IndexInfo {
    std::string name;
    std::string table;
    std::vector<std::size_t> columns;  // positions in the table schema
    bool unique = false;
    std::unique_ptr<index::BPlusTree> tree;

    [[nodiscard]] index::Key keyFor(const Row& row) const;
    [[nodiscard]] std::string definition(const TableInfo& t) const;
};

struct TableInfo {
    std::uint32_t id = 0;
    std::string name;
    std::vector<ColumnInfo> columns;
    std::unique_ptr<storage::TableHeap> heap;
    std::vector<ColumnStats> stats;
    std::vector<IndexInfo*> indexes;
    std::uint64_t version = 0;  // bumped on every modification

    [[nodiscard]] std::optional<std::size_t> columnIndex(const std::string& col) const;
    [[nodiscard]] std::size_t rowCount() const { return heap->rowCount(); }
    [[nodiscard]] std::size_t pageCount() const { return heap->pages().size(); }
};

/// Owns tables and indexes and keeps heap, indexes and statistics consistent.
class Catalog {
public:
    explicit Catalog(storage::BufferPool& pool) : pool_(&pool) {}

    TableInfo& createTable(const std::string& name, std::vector<ColumnInfo> columns);
    void dropTable(const std::string& name);
    IndexInfo& createIndex(const std::string& name, const std::string& table, const std::vector<std::string>& columns,
                           bool unique);
    void dropIndex(const std::string& name);

    [[nodiscard]] TableInfo* findTable(const std::string& name) const;
    [[nodiscard]] IndexInfo* findIndex(const std::string& name) const;
    [[nodiscard]] std::vector<TableInfo*> tables() const;
    [[nodiscard]] std::vector<IndexInfo*> indexes() const;

    /// Row-level mutations: heap + every index + statistics.
    storage::Rid insertRow(TableInfo& t, const Row& row);
    void deleteRow(TableInfo& t, const storage::Rid& rid, const Row& row);
    storage::Rid updateRow(TableInfo& t, const storage::Rid& rid, const Row& oldRow, const Row& newRow);

    /// Persistence support: re-create a table around existing pages.
    TableInfo& restoreTable(std::uint32_t id, const std::string& name, std::vector<ColumnInfo> columns,
                            std::vector<storage::PageId> pages, std::size_t rowCount);
    /// Recomputes statistics from the heap (after restoring from disk).
    void rebuildStats(TableInfo& t);

    [[nodiscard]] storage::BufferPool& pool() { return *pool_; }
    [[nodiscard]] std::uint64_t schemaVersion() const { return schemaVersion_; }

private:
    void checkUnique(const TableInfo& t, const Row& row, const storage::Rid* ignore) const;

    storage::BufferPool* pool_;
    std::map<std::string, std::unique_ptr<TableInfo>> tables_;
    std::map<std::string, std::unique_ptr<IndexInfo>> indexes_;
    std::uint32_t nextTableId_ = 1;
    std::uint64_t schemaVersion_ = 0;
};

}  // namespace querylab::catalog

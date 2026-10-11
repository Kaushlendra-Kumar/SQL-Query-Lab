#pragma once

#include <functional>
#include <vector>

#include "querylab/storage/buffer_pool.h"

namespace querylab::storage {

/// Unordered collection of rows stored in slotted pages (a "heap file").
/// New rows go to the last page; a new page is allocated when it is full.
class TableHeap {
public:
    TableHeap(BufferPool& pool, std::size_t columnCount) : pool_(&pool), columnCount_(columnCount) {}

    Rid insert(const Row& row);
    /// Reads one row through the buffer pool.
    [[nodiscard]] Row fetch(const Rid& rid) const;
    void fetchColumns(const Rid& rid, const std::vector<bool>& wanted, Row& out) const;
    void erase(const Rid& rid);
    /// Updates in place when the new image fits, otherwise moves the row.
    /// Returns the (possibly new) RID.
    Rid update(const Rid& rid, const Row& row);

    [[nodiscard]] const std::vector<PageId>& pages() const { return pages_; }
    [[nodiscard]] std::size_t rowCount() const { return rowCount_; }
    [[nodiscard]] std::size_t columnCount() const { return columnCount_; }

    /// Used when loading from disk.
    void restore(std::vector<PageId> pages, std::size_t rowCount) {
        pages_ = std::move(pages);
        rowCount_ = rowCount;
    }

    /// Calls fn(rid, row) for every live row (full decode). Used by index
    /// builds and statistics, not by query execution (operators scan pages
    /// themselves so they can account for page reads).
    void forEach(const std::function<void(const Rid&, const Row&)>& fn) const;

private:
    BufferPool* pool_;
    std::size_t columnCount_;
    std::vector<PageId> pages_;
    std::size_t rowCount_ = 0;
};

}  // namespace querylab::storage

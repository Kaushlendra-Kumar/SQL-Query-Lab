#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "querylab/common/value.h"
#include "querylab/storage/page.h"

namespace querylab::index {

using storage::Rid;
using Key = Row;  // composite key; one Value per indexed column

struct IndexScanStats {
    std::uint64_t nodesVisited = 0;      // internal + leaf nodes touched
    std::uint64_t entriesExamined = 0;   // leaf entries compared/returned
};

/// In-memory B+ tree mapping composite keys to RIDs.
///
/// Duplicate keys are handled by ordering entries on (key, rid): every entry
/// is unique, equal keys are adjacent in the leaf chain, and deleting one
/// duplicate is exact. Leaves are linked for range scans.
///
/// Deletion removes entries without rebalancing (underfull nodes are allowed,
/// as in several production systems that rely on later rebuilds). Lookups stay
/// correct because separator invariants are preserved.
class BPlusTree {
public:
    struct Entry {
        Key key;
        Rid rid;
    };

    explicit BPlusTree(std::size_t keyColumns, std::size_t maxEntriesPerNode = 64);
    ~BPlusTree();
    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    void insert(const Key& key, const Rid& rid);
    /// Removes the exact (key, rid) entry; returns false if absent.
    bool erase(const Key& key, const Rid& rid);
    /// True if some entry has exactly this key.
    [[nodiscard]] bool containsKey(const Key& key) const;

    class Cursor {
    public:
        [[nodiscard]] bool valid() const { return leaf_ != nullptr; }
        [[nodiscard]] const Entry& entry() const;
        void next(IndexScanStats& stats);

    private:
        friend class BPlusTree;
        const void* leaf_ = nullptr;
        std::size_t idx_ = 0;
        void settle(IndexScanStats& stats);
    };

    /// Positions at the first entry whose key prefix is >= `lowPrefix`
    /// (or > when !inclusive). An empty prefix starts at the smallest key.
    [[nodiscard]] Cursor seek(const Row& lowPrefix, bool inclusive, IndexScanStats& stats) const;

    /// Compares the first prefix.size() columns of `key` with `prefix`.
    static int comparePrefix(const Key& key, const Row& prefix);

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] std::size_t height() const { return height_; }
    [[nodiscard]] std::size_t nodeCount() const;
    [[nodiscard]] std::size_t leafCount() const;
    [[nodiscard]] std::size_t keyColumns() const { return keyColumns_; }
    [[nodiscard]] std::size_t fanout() const { return maxEntries_; }
    [[nodiscard]] std::size_t approximateBytes() const;

    /// Checks ordering, separator bounds, leaf links and size; returns an
    /// empty string when the tree is valid.
    [[nodiscard]] std::string validate() const;

    /// Structural summary for the storage explorer (first levels / leaves).
    [[nodiscard]] nlohmann::json describe(std::size_t maxNodesPerLevel = 12) const;

    /// All entries in order (tests / debugging).
    [[nodiscard]] std::vector<Entry> entries() const;

private:
    struct Node;
    static int compareEntry(const Key& ak, const Rid& ar, const Key& bk, const Rid& br);
    struct SplitResult;
    std::unique_ptr<SplitResult> insertInto(Node* node, const Key& key, const Rid& rid);

    std::unique_ptr<Node> root_;
    std::size_t keyColumns_;
    std::size_t maxEntries_;
    std::size_t size_ = 0;
    std::size_t height_ = 1;
};

}  // namespace querylab::index

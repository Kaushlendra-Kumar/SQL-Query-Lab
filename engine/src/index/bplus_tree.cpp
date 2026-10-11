#include "querylab/index/bplus_tree.h"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace querylab::index {

struct BPlusTree::Node {
    bool leaf = true;
    // Leaf: sorted entries. Internal: separators (entries[i] is the smallest
    // entry reachable through children[i + 1]).
    std::vector<Entry> entries;
    std::vector<std::unique_ptr<Node>> children;
    Node* next = nullptr;  // leaf chain
};

struct BPlusTree::SplitResult {
    Entry separator;
    std::unique_ptr<Node> right;
};

BPlusTree::BPlusTree(std::size_t keyColumns, std::size_t maxEntriesPerNode)
    : root_(std::make_unique<Node>()), keyColumns_(keyColumns), maxEntries_(std::max<std::size_t>(maxEntriesPerNode, 4)) {}

BPlusTree::~BPlusTree() = default;

int BPlusTree::compareEntry(const Key& ak, const Rid& ar, const Key& bk, const Rid& br) {
    const int c = compareRows(ak, bk);
    if (c != 0) return c;
    if (ar < br) return -1;
    return ar == br ? 0 : 1;
}

int BPlusTree::comparePrefix(const Key& key, const Row& prefix) {
    for (std::size_t i = 0; i < prefix.size() && i < key.size(); ++i) {
        const int c = Value::compareTotal(key[i], prefix[i]);
        if (c != 0) return c;
    }
    return 0;
}

// Number of separators <= (key, rid): index of the child that holds it.
static std::size_t routeExact(const std::vector<BPlusTree::Entry>& seps, const Key& key, const Rid& rid,
                              int (*cmp)(const Key&, const Rid&, const Key&, const Rid&)) {
    std::size_t lo = 0, hi = seps.size();
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (cmp(seps[mid].key, seps[mid].rid, key, rid) <= 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

std::unique_ptr<BPlusTree::SplitResult> BPlusTree::insertInto(Node* node, const Key& key, const Rid& rid) {
    if (node->leaf) {
        auto it = std::lower_bound(node->entries.begin(), node->entries.end(), 0, [&](const Entry& e, int) {
            return compareEntry(e.key, e.rid, key, rid) < 0;
        });
        if (it != node->entries.end() && compareEntry(it->key, it->rid, key, rid) == 0) {
            throw std::logic_error("duplicate (key, rid) in B+ tree");
        }
        node->entries.insert(it, Entry{key, rid});
        if (node->entries.size() <= maxEntries_) return nullptr;
        auto right = std::make_unique<Node>();
        right->leaf = true;
        const std::size_t mid = node->entries.size() / 2;
        right->entries.assign(std::make_move_iterator(node->entries.begin() + static_cast<std::ptrdiff_t>(mid)),
                              std::make_move_iterator(node->entries.end()));
        node->entries.resize(mid);
        right->next = node->next;
        node->next = right.get();
        auto res = std::make_unique<SplitResult>();
        res->separator = right->entries.front();
        res->right = std::move(right);
        return res;
    }

    const std::size_t ci = routeExact(node->entries, key, rid, &compareEntry);
    auto split = insertInto(node->children[ci].get(), key, rid);
    if (!split) return nullptr;
    node->entries.insert(node->entries.begin() + static_cast<std::ptrdiff_t>(ci), std::move(split->separator));
    node->children.insert(node->children.begin() + static_cast<std::ptrdiff_t>(ci) + 1, std::move(split->right));
    if (node->entries.size() <= maxEntries_) return nullptr;

    // Split internal node: the middle separator moves up.
    auto right = std::make_unique<Node>();
    right->leaf = false;
    const std::size_t mid = node->entries.size() / 2;
    auto res = std::make_unique<SplitResult>();
    res->separator = std::move(node->entries[mid]);
    right->entries.assign(std::make_move_iterator(node->entries.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
                          std::make_move_iterator(node->entries.end()));
    right->children.assign(std::make_move_iterator(node->children.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
                           std::make_move_iterator(node->children.end()));
    node->entries.resize(mid);
    node->children.resize(mid + 1);
    res->right = std::move(right);
    return res;
}

void BPlusTree::insert(const Key& key, const Rid& rid) {
    if (key.size() != keyColumns_) throw std::logic_error("B+ tree key arity mismatch");
    auto split = insertInto(root_.get(), key, rid);
    if (split) {
        auto newRoot = std::make_unique<Node>();
        newRoot->leaf = false;
        newRoot->entries.push_back(std::move(split->separator));
        newRoot->children.push_back(std::move(root_));
        newRoot->children.push_back(std::move(split->right));
        root_ = std::move(newRoot);
        ++height_;
    }
    ++size_;
}

bool BPlusTree::erase(const Key& key, const Rid& rid) {
    Node* n = root_.get();
    while (!n->leaf) n = n->children[routeExact(n->entries, key, rid, &compareEntry)].get();
    auto it = std::lower_bound(n->entries.begin(), n->entries.end(), 0, [&](const Entry& e, int) {
        return compareEntry(e.key, e.rid, key, rid) < 0;
    });
    if (it == n->entries.end() || compareEntry(it->key, it->rid, key, rid) != 0) return false;
    n->entries.erase(it);
    --size_;
    return true;
}

const BPlusTree::Entry& BPlusTree::Cursor::entry() const {
    return static_cast<const Node*>(leaf_)->entries[idx_];
}

void BPlusTree::Cursor::settle(IndexScanStats& stats) {
    // Skip past exhausted (possibly empty) leaves.
    while (leaf_ && idx_ >= static_cast<const Node*>(leaf_)->entries.size()) {
        leaf_ = static_cast<const Node*>(leaf_)->next;
        idx_ = 0;
        if (leaf_) ++stats.nodesVisited;
    }
    if (leaf_) ++stats.entriesExamined;
}

void BPlusTree::Cursor::next(IndexScanStats& stats) {
    if (!leaf_) return;
    ++idx_;
    settle(stats);
}

BPlusTree::Cursor BPlusTree::seek(const Row& lowPrefix, bool inclusive, IndexScanStats& stats) const {
    const Node* n = root_.get();
    ++stats.nodesVisited;
    auto qualifies = [&](const Entry& e) {
        const int c = comparePrefix(e.key, lowPrefix);
        return inclusive ? c >= 0 : c > 0;
    };
    while (!n->leaf) {
        // Leftmost child that may contain a qualifying entry.
        std::size_t ci = 0;
        while (ci < n->entries.size() && !qualifies(n->entries[ci])) ++ci;
        n = n->children[ci].get();
        ++stats.nodesVisited;
    }
    Cursor c;
    c.leaf_ = n;
    c.idx_ = static_cast<std::size_t>(
        std::partition_point(n->entries.begin(), n->entries.end(), [&](const Entry& e) { return !qualifies(e); }) -
        n->entries.begin());
    c.settle(stats);
    // Entries in later leaves are all >= this leaf's, but an empty-prefix
    // seek or a seek landing in an emptied leaf may need to advance.
    while (c.valid() && !qualifies(c.entry())) c.next(stats);
    return c;
}

bool BPlusTree::containsKey(const Key& key) const {
    IndexScanStats s;
    Cursor c = seek(key, true, s);
    return c.valid() && compareRows(c.entry().key, key) == 0;
}

std::size_t BPlusTree::nodeCount() const {
    std::function<std::size_t(const Node*)> count = [&](const Node* n) -> std::size_t {
        std::size_t total = 1;
        for (const auto& ch : n->children) total += count(ch.get());
        return total;
    };
    return count(root_.get());
}

std::size_t BPlusTree::leafCount() const {
    const Node* n = root_.get();
    while (!n->leaf) n = n->children.front().get();
    std::size_t c = 0;
    for (; n; n = n->next) ++c;
    return c;
}

std::size_t BPlusTree::approximateBytes() const {
    std::size_t bytes = nodeCount() * sizeof(Node);
    for (const auto& e : entries()) {
        bytes += sizeof(Entry);
        for (const auto& v : e.key) bytes += v.footprint();
    }
    return bytes;
}

std::vector<BPlusTree::Entry> BPlusTree::entries() const {
    std::vector<Entry> out;
    out.reserve(size_);
    const Node* n = root_.get();
    while (!n->leaf) n = n->children.front().get();
    for (; n; n = n->next) out.insert(out.end(), n->entries.begin(), n->entries.end());
    return out;
}

std::string BPlusTree::validate() const {
    std::string err;
    std::size_t counted = 0;
    std::vector<const Node*> leavesInOrder;
    std::function<void(const Node*, const Entry*, const Entry*, std::size_t)> check =
        [&](const Node* n, const Entry* lo, const Entry* hi, std::size_t depth) {
            if (!err.empty()) return;
            for (std::size_t i = 1; i < n->entries.size(); ++i) {
                if (compareEntry(n->entries[i - 1].key, n->entries[i - 1].rid, n->entries[i].key, n->entries[i].rid) >= 0) {
                    err = "entries out of order";
                    return;
                }
            }
            if (n->entries.size() > maxEntries_) err = "node overflow";
            if (n->leaf) {
                if (depth != height_) err = "leaves at different depths";
                for (const auto& e : n->entries) {
                    if (lo && compareEntry(e.key, e.rid, lo->key, lo->rid) < 0) err = "entry below lower separator";
                    if (hi && compareEntry(e.key, e.rid, hi->key, hi->rid) >= 0) err = "entry not below upper separator";
                }
                counted += n->entries.size();
                leavesInOrder.push_back(n);
                return;
            }
            if (n->children.size() != n->entries.size() + 1) {
                err = "child count mismatch";
                return;
            }
            for (std::size_t i = 0; i < n->children.size(); ++i) {
                const Entry* clo = i == 0 ? lo : &n->entries[i - 1];
                const Entry* chi = i == n->entries.size() ? hi : &n->entries[i];
                check(n->children[i].get(), clo, chi, depth + 1);
            }
        };
    check(root_.get(), nullptr, nullptr, 1);
    if (!err.empty()) return err;
    if (counted != size_) return "size mismatch";
    for (std::size_t i = 0; i + 1 < leavesInOrder.size(); ++i) {
        if (leavesInOrder[i]->next != leavesInOrder[i + 1]) return "broken leaf chain";
    }
    if (!leavesInOrder.empty() && leavesInOrder.back()->next != nullptr) return "leaf chain does not terminate";
    return "";
}

nlohmann::json BPlusTree::describe(std::size_t maxNodesPerLevel) const {
    using nlohmann::json;
    auto keyText = [](const Key& k) {
        std::string s;
        for (std::size_t i = 0; i < k.size(); ++i) {
            if (i) s += ", ";
            s += k[i].toSqlLiteral();
        }
        return k.size() == 1 ? s : "(" + s + ")";
    };
    json levels = json::array();
    std::vector<const Node*> level{root_.get()};
    std::size_t depth = 0;
    while (!level.empty()) {
        json nodes = json::array();
        std::vector<const Node*> nextLevel;
        std::size_t totalAtLevel = level.size();
        for (std::size_t i = 0; i < level.size(); ++i) {
            const Node* n = level[i];
            if (i < maxNodesPerLevel) {
                json keys = json::array();
                const std::size_t shown = std::min<std::size_t>(n->entries.size(), 6);
                for (std::size_t k = 0; k < shown; ++k) keys.push_back(keyText(n->entries[k].key));
                nodes.push_back({{"leaf", n->leaf}, {"entryCount", n->entries.size()}, {"keys", keys},
                                 {"truncated", n->entries.size() > shown}});
            }
            for (const auto& ch : n->children) nextLevel.push_back(ch.get());
        }
        levels.push_back({{"depth", depth}, {"nodeCount", totalAtLevel}, {"nodes", nodes}, {"leaf", level.front()->leaf}});
        level = std::move(nextLevel);
        ++depth;
    }
    return json{{"height", height_}, {"entries", size_}, {"fanout", maxEntries_}, {"nodeCount", nodeCount()},
                {"leafCount", leafCount()}, {"levels", levels}};
}

}  // namespace querylab::index

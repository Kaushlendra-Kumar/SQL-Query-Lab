// Pages, buffer pool, table heap, B+ tree and statistics.
#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>

#include "querylab/api/datasets.h"
#include "querylab/catalog/catalog.h"
#include "querylab/common/error.h"
#include "querylab/index/bplus_tree.h"
#include "querylab/storage/buffer_pool.h"
#include "querylab/storage/table_heap.h"

using namespace querylab;
using namespace querylab::storage;

TEST_CASE("row codec round-trips every type and skips unwanted columns") {
    Row r{Value::integer(-42), Value::floating(3.25), Value::text("it's ünïcode"), Value::boolean(true), Value::null(), Value::text("")};
    auto bytes = codec::encodeRow(r);
    Row back = codec::decodeRow(bytes, r.size());
    CHECK(compareRows(r, back) == 0);
    Row partial;
    codec::decodeColumns(bytes, {false, true, false, false, false, false}, partial);
    CHECK(partial[0].isNull());
    CHECK(partial[1].asFloat() == 3.25);
    CHECK(partial[2].isNull());
    CHECK_THROWS_AS(codec::decodeRow(std::span<const std::uint8_t>(bytes.data(), 5), r.size()), querylab::QueryError);
}

TEST_CASE("slotted page insert, erase, in-place update and checksum") {
    Page p;
    p.init(7);
    CHECK(p.id() == 7);
    std::vector<std::uint8_t> a(100, 1), b(50, 2);
    auto s0 = p.insert(a);
    auto s1 = p.insert(b);
    REQUIRE(s0);
    REQUIRE(s1);
    CHECK(p.get(*s1).size() == 50);
    p.erase(*s0);
    CHECK_FALSE(p.isLive(*s0));
    CHECK(p.liveCount() == 1);
    CHECK_THROWS_AS((void)p.get(*s0), querylab::QueryError);
    std::vector<std::uint8_t> smaller(10, 3), bigger(80, 4);
    CHECK(p.updateInPlace(*s1, smaller));
    CHECK(p.get(*s1).size() == 10);
    CHECK_FALSE(p.updateInPlace(*s1, bigger));
    // Fill until full.
    int n = 0;
    while (p.insert(a)) ++n;
    CHECK(n > 30);
    CHECK(p.freeSpace() < 104);
    p.stampChecksum();
    CHECK(p.verifyChecksum());
    p.raw()[200] ^= 0xFF;
    CHECK_FALSE(p.verifyChecksum());
    std::vector<std::uint8_t> huge(Page::kMaxRowSize + 1, 0);
    Page empty;
    empty.init(1);
    CHECK_FALSE(empty.insert(huge));
}

TEST_CASE("buffer pool: hits, misses, LRU eviction and write-back") {
    MemoryPageStore store;
    BufferPool pool(store, 4);
    std::vector<PageId> ids;
    for (int i = 0; i < 6; ++i) {
        PageId id;
        auto g = pool.newPage(id);
        std::vector<std::uint8_t> bytes(8, static_cast<std::uint8_t>(i));
        g.mutablePage().insert(bytes);
        ids.push_back(id);
    }
    CHECK(pool.cachedPages() == 4);
    CHECK(pool.stats().evictions == 2);
    CHECK(pool.stats().writes == 2);  // evicted dirty pages were written back
    const auto before = pool.stats();
    { auto g = pool.fetch(ids[0]); CHECK(g.page().get(0)[0] == 0); }  // miss: evicted earlier
    { auto g = pool.fetch(ids[5]); }                                  // hit
    const auto d = pool.stats() - before;
    CHECK(d.misses == 1);
    CHECK(d.hits == 1);
    // Pinned pages are never evicted.
    std::vector<PageGuard> pins;
    for (int i = 0; i < 4; ++i) pins.push_back(pool.fetch(ids[static_cast<std::size_t>(i)]));
    CHECK_THROWS_AS(pool.fetch(ids[5]), querylab::QueryError);
    pins.clear();
    pool.evictAll();
    CHECK(pool.cachedPages() == 0);
    auto g = pool.fetch(ids[3]);
    CHECK(g.page().get(0)[0] == 3);
}

TEST_CASE("table heap: stable RIDs, deletes and moving updates") {
    MemoryPageStore store;
    BufferPool pool(store, 8);
    TableHeap heap(pool, 2);
    std::vector<Rid> rids;
    for (int i = 0; i < 500; ++i) rids.push_back(heap.insert({Value::integer(i), Value::text(std::string(20, 'x'))}));
    CHECK(heap.rowCount() == 500);
    CHECK(heap.pages().size() > 2);
    CHECK(heap.fetch(rids[123])[0].asInt() == 123);
    heap.erase(rids[10]);
    CHECK(heap.rowCount() == 499);
    CHECK_THROWS_AS((void)heap.fetch(rids[10]), querylab::QueryError);
    CHECK(heap.fetch(rids[11])[0].asInt() == 11);  // other RIDs unaffected
    // Growing update must move the row.
    Rid moved = heap.update(rids[20], {Value::integer(20), Value::text(std::string(300, 'y'))});
    CHECK_FALSE(moved == rids[20]);
    CHECK(heap.fetch(moved)[1].asText().size() == 300);
    Rid same = heap.update(rids[30], {Value::integer(31), Value::text("short")});
    CHECK(same == rids[30]);
    std::size_t count = 0;
    heap.forEach([&](const Rid&, const Row&) { ++count; });
    CHECK(count == 499);
    CHECK_THROWS_AS(heap.insert({Value::integer(1), Value::text(std::string(5000, 'z'))}), querylab::QueryError);
}

TEST_CASE("B+ tree: splits, duplicates, range scans and deletion (randomized)") {
    index::BPlusTree tree(1, 8);  // small fanout to force many splits
    api::Rng rng(7);
    std::multimap<std::int64_t, Rid> reference;
    for (std::uint16_t i = 0; i < 3000; ++i) {
        const auto k = rng.range(0, 400);  // many duplicates
        const Rid rid{static_cast<PageId>(i / 50), static_cast<std::uint16_t>(i % 50)};
        tree.insert({Value::integer(k)}, rid);
        reference.emplace(k, rid);
    }
    CHECK(tree.validate().empty());
    CHECK(tree.height() >= 3);
    CHECK(tree.size() == 3000);

    auto rangeCount = [&](std::int64_t lo, std::int64_t hi) {
        index::IndexScanStats st;
        auto c = tree.seek({Value::integer(lo)}, true, st);
        std::size_t n = 0;
        while (c.valid() && c.entry().key[0].asInt() <= hi) {
            ++n;
            c.next(st);
        }
        return n;
    };
    for (int trial = 0; trial < 50; ++trial) {
        const auto lo = rng.range(-10, 410);
        const auto hi = lo + rng.range(0, 40);
        const auto expected = static_cast<std::size_t>(std::distance(reference.lower_bound(lo), reference.upper_bound(hi)));
        CHECK(rangeCount(lo, hi) == expected);
    }
    // Exclusive seek.
    {
        index::IndexScanStats st;
        auto c = tree.seek({Value::integer(100)}, false, st);
        REQUIRE(c.valid());
        CHECK(c.entry().key[0].asInt() > 100);
        CHECK(st.nodesVisited >= tree.height());
    }
    // Delete half the entries, including duplicates of the same key.
    std::size_t removed = 0;
    for (auto it = reference.begin(); it != reference.end();) {
        if (rng.range(0, 1) == 0) {
            CHECK(tree.erase({Value::integer(it->first)}, it->second));
            it = reference.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    CHECK_FALSE(tree.erase({Value::integer(99999)}, Rid{0, 0}));
    CHECK(tree.size() == 3000 - removed);
    CHECK(tree.validate().empty());
    for (int trial = 0; trial < 50; ++trial) {
        const auto lo = rng.range(-10, 410);
        const auto hi = lo + rng.range(0, 40);
        const auto expected = static_cast<std::size_t>(std::distance(reference.lower_bound(lo), reference.upper_bound(hi)));
        CHECK(rangeCount(lo, hi) == expected);
    }
    // Entries come back in key order.
    auto all = tree.entries();
    CHECK(std::is_sorted(all.begin(), all.end(), [](const auto& a, const auto& b) { return compareRows(a.key, b.key) < 0; }));
    // Delete everything: the tree stays valid and empty.
    for (const auto& [k, rid] : reference) tree.erase({Value::integer(k)}, rid);
    CHECK(tree.size() == 0);
    CHECK(tree.validate().empty());
    index::IndexScanStats st;
    CHECK_FALSE(tree.seek({}, true, st).valid());
}

TEST_CASE("B+ tree: composite keys and prefix seeks") {
    index::BPlusTree tree(2, 4);
    std::uint16_t slot = 0;
    for (int a = 0; a < 20; ++a) {
        for (int b = 0; b < 10; ++b) tree.insert({Value::integer(a), Value::text("k" + std::to_string(b))}, Rid{0, slot++});
    }
    CHECK(tree.validate().empty());
    index::IndexScanStats st;
    auto c = tree.seek({Value::integer(7)}, true, st);
    int n = 0;
    while (c.valid() && index::BPlusTree::comparePrefix(c.entry().key, {Value::integer(7)}) == 0) {
        ++n;
        c.next(st);
    }
    CHECK(n == 10);
    CHECK(tree.containsKey({Value::integer(3), Value::text("k4")}));
    CHECK_FALSE(tree.containsKey({Value::integer(3), Value::text("k99")}));
}

TEST_CASE("column statistics and histogram fractions") {
    catalog::ColumnStats s;
    for (int i = 1; i <= 1000; ++i) s.add(Value::integer(i));
    s.add(Value::null());
    CHECK(s.distinct() == 1000);
    CHECK(s.nullCount() == 1);
    CHECK(s.min()->asInt() == 1);
    CHECK(s.max()->asInt() == 1000);
    CHECK(s.fractionBelow(Value::integer(500), true) == doctest::Approx(0.5).epsilon(0.05));
    CHECK(s.fractionBelow(Value::integer(0), true) == doctest::Approx(0.0));
    CHECK(s.fractionBelow(Value::integer(2000), false) == doctest::Approx(1.0));
    s.remove(Value::integer(1000));
    CHECK(s.max()->asInt() == 999);
}

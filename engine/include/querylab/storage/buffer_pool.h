#pragma once

#include <cstdint>
#include <fstream>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "querylab/storage/page.h"

namespace querylab::storage {

/// The "disk": where pages live when they are not cached in the buffer pool.
class PageStore {
public:
    virtual ~PageStore() = default;
    virtual PageId allocate() = 0;
    virtual void read(PageId id, Page& out) = 0;
    virtual void write(PageId id, const Page& page) = 0;
    [[nodiscard]] virtual std::size_t pageCount() const = 0;
    virtual void sync() {}
    [[nodiscard]] virtual bool persistent() const = 0;
};

/// In-memory page store. Reads copy a page into a buffer-pool frame, so cache
/// hits/misses are real events even though no disk I/O happens.
class MemoryPageStore final : public PageStore {
public:
    PageId allocate() override;
    void read(PageId id, Page& out) override;
    void write(PageId id, const Page& page) override;
    [[nodiscard]] std::size_t pageCount() const override { return pages_.size(); }
    [[nodiscard]] bool persistent() const override { return false; }

private:
    std::vector<std::unique_ptr<Page>> pages_;
};

/// File-backed page store: page N lives at byte offset N * kPageSize.
/// Page 0 is reserved for the database file header (see persistence.cpp).
class FilePageStore final : public PageStore {
public:
    /// Opens an existing file or creates a new one. Throws QueryError(Storage).
    explicit FilePageStore(const std::string& path, bool create);
    PageId allocate() override;
    void read(PageId id, Page& out) override;
    void write(PageId id, const Page& page) override;
    [[nodiscard]] std::size_t pageCount() const override { return pageCount_; }
    void sync() override;
    [[nodiscard]] bool persistent() const override { return true; }
    void readRaw(PageId id, Page& out);  // no checksum verification
    void writeRaw(PageId id, const Page& page);

private:
    std::string path_;
    std::fstream file_;
    std::size_t pageCount_ = 0;
};

struct BufferPoolStats {
    std::uint64_t logicalReads = 0;  // fetch() calls
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;        // page had to be read from the PageStore
    std::uint64_t evictions = 0;
    std::uint64_t writes = 0;        // dirty pages written back
    BufferPoolStats operator-(const BufferPoolStats& o) const {
        return {logicalReads - o.logicalReads, hits - o.hits, misses - o.misses, evictions - o.evictions, writes - o.writes};
    }
};

class BufferPool;

/// RAII pin on a buffered page. The page cannot be evicted while pinned.
class PageGuard {
public:
    PageGuard() = default;
    PageGuard(BufferPool* pool, std::size_t frame, Page* page) : pool_(pool), frame_(frame), page_(page) {}
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    PageGuard(PageGuard&& o) noexcept { *this = std::move(o); }
    PageGuard& operator=(PageGuard&& o) noexcept;
    ~PageGuard() { release(); }

    [[nodiscard]] const Page& page() const { return *page_; }
    Page& mutablePage();  // marks the frame dirty
    void release();

private:
    BufferPool* pool_ = nullptr;
    std::size_t frame_ = 0;
    Page* page_ = nullptr;
};

/// Bounded LRU page cache in front of a PageStore.
class BufferPool {
public:
    BufferPool(PageStore& store, std::size_t capacity);

    PageGuard fetch(PageId id);
    PageGuard newPage(PageId& outId);
    /// Copies a page for inspection without touching the cache: no stats,
    /// no LRU update, and an uncached page is read from the store directly
    /// instead of being loaded into a frame. Used by the storage explorer so
    /// looking at pages never changes the outcome of an experiment.
    void peek(PageId id, Page& out);

    void flushAll();
    /// Writes back and drops every cached page ("cold cache"). Requires that
    /// no page is pinned.
    void evictAll();

    [[nodiscard]] const BufferPoolStats& stats() const { return stats_; }
    [[nodiscard]] std::size_t capacity() const { return frames_.size(); }
    [[nodiscard]] std::size_t cachedPages() const { return table_.size(); }
    [[nodiscard]] bool isCached(PageId id) const { return table_.count(id) != 0; }
    [[nodiscard]] PageStore& store() { return store_; }
    void resize(std::size_t capacity);

private:
    friend class PageGuard;
    struct Frame {
        Page page;
        PageId id = kInvalidPageId;
        int pins = 0;
        bool dirty = false;
        std::list<std::size_t>::iterator lruPos;
        bool inLru = false;
    };

    std::size_t acquireFrame();
    void unpin(std::size_t frame);
    void markDirty(std::size_t frame) { frames_[frame]->dirty = true; }
    void touch(std::size_t frame);

    PageStore& store_;
    std::vector<std::unique_ptr<Frame>> frames_;
    std::vector<std::size_t> freeFrames_;
    std::unordered_map<PageId, std::size_t> table_;
    std::list<std::size_t> lru_;  // front = most recently used, unpinned frames only
    BufferPoolStats stats_;
};

}  // namespace querylab::storage

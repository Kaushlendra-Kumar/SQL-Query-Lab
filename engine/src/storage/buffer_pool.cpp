#include "querylab/storage/buffer_pool.h"

#include <filesystem>
#include <stdexcept>

#include "querylab/common/error.h"

namespace querylab::storage {

// ---- MemoryPageStore -------------------------------------------------------

PageId MemoryPageStore::allocate() {
    const auto id = static_cast<PageId>(pages_.size());
    auto p = std::make_unique<Page>();
    p->init(id);
    pages_.push_back(std::move(p));
    return id;
}

void MemoryPageStore::read(PageId id, Page& out) {
    if (id >= pages_.size()) throw QueryError(ErrorStage::Storage, "page " + std::to_string(id) + " does not exist");
    out = *pages_[id];
}

void MemoryPageStore::write(PageId id, const Page& page) {
    if (id >= pages_.size()) throw QueryError(ErrorStage::Storage, "page " + std::to_string(id) + " does not exist");
    *pages_[id] = page;
}

// ---- FilePageStore ---------------------------------------------------------

FilePageStore::FilePageStore(const std::string& path, bool create) : path_(path) {
    namespace fs = std::filesystem;
    if (create) {
        std::ofstream(path, std::ios::binary | std::ios::trunc).close();
    } else if (!fs::exists(path)) {
        throw QueryError(ErrorStage::Storage, "database file '" + path + "' does not exist");
    }
    file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_) throw QueryError(ErrorStage::Storage, "cannot open database file '" + path + "'");
    const auto size = fs::file_size(path);
    if (size % kPageSize != 0) {
        throw QueryError(ErrorStage::Storage, "database file '" + path + "' is truncated or corrupted (size " +
                                                  std::to_string(size) + " is not a multiple of the page size)");
    }
    pageCount_ = static_cast<std::size_t>(size / kPageSize);
}

PageId FilePageStore::allocate() {
    const auto id = static_cast<PageId>(pageCount_);
    Page p;
    p.init(id);
    ++pageCount_;
    write(id, p);
    return id;
}

void FilePageStore::readRaw(PageId id, Page& out) {
    if (id >= pageCount_) throw QueryError(ErrorStage::Storage, "page " + std::to_string(id) + " is beyond the end of the file");
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(id) * static_cast<std::streamoff>(kPageSize));
    file_.read(reinterpret_cast<char*>(out.raw()), kPageSize);
    if (!file_) throw QueryError(ErrorStage::Storage, "short read on page " + std::to_string(id));
}

void FilePageStore::read(PageId id, Page& out) {
    readRaw(id, out);
    if (!out.verifyChecksum() || out.id() != id) {
        throw QueryError(ErrorStage::Storage, "checksum mismatch on page " + std::to_string(id) + " (file is corrupted)");
    }
}

void FilePageStore::writeRaw(PageId id, const Page& page) {
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(id) * static_cast<std::streamoff>(kPageSize));
    file_.write(reinterpret_cast<const char*>(page.raw()), kPageSize);
    if (!file_) throw QueryError(ErrorStage::Storage, "write failed on page " + std::to_string(id));
}

void FilePageStore::write(PageId id, const Page& page) {
    Page copy = page;
    copy.stampChecksum();
    writeRaw(id, copy);
}

void FilePageStore::sync() { file_.flush(); }

// ---- PageGuard -------------------------------------------------------------

PageGuard& PageGuard::operator=(PageGuard&& o) noexcept {
    if (this != &o) {
        release();
        pool_ = o.pool_;
        frame_ = o.frame_;
        page_ = o.page_;
        o.pool_ = nullptr;
        o.page_ = nullptr;
    }
    return *this;
}

Page& PageGuard::mutablePage() {
    pool_->markDirty(frame_);
    return *page_;
}

void PageGuard::release() {
    if (pool_) {
        pool_->unpin(frame_);
        pool_ = nullptr;
        page_ = nullptr;
    }
}

// ---- BufferPool ------------------------------------------------------------

BufferPool::BufferPool(PageStore& store, std::size_t capacity) : store_(store) {
    if (capacity < 4) capacity = 4;
    for (std::size_t i = 0; i < capacity; ++i) {
        frames_.push_back(std::make_unique<Frame>());
        freeFrames_.push_back(capacity - 1 - i);
    }
}

void BufferPool::touch(std::size_t f) {
    Frame& fr = *frames_[f];
    if (fr.inLru) lru_.erase(fr.lruPos);
    fr.inLru = false;
}

std::size_t BufferPool::acquireFrame() {
    if (!freeFrames_.empty()) {
        const std::size_t f = freeFrames_.back();
        freeFrames_.pop_back();
        return f;
    }
    if (lru_.empty()) {
        throw QueryError(ErrorStage::Storage, "buffer pool exhausted: all " + std::to_string(frames_.size()) + " frames are pinned");
    }
    const std::size_t victim = lru_.back();
    lru_.pop_back();
    Frame& fr = *frames_[victim];
    fr.inLru = false;
    if (fr.dirty) {
        store_.write(fr.id, fr.page);
        ++stats_.writes;
        fr.dirty = false;
    }
    table_.erase(fr.id);
    fr.id = kInvalidPageId;
    ++stats_.evictions;
    return victim;
}

PageGuard BufferPool::fetch(PageId id) {
    ++stats_.logicalReads;
    auto it = table_.find(id);
    if (it != table_.end()) {
        ++stats_.hits;
        Frame& fr = *frames_[it->second];
        touch(it->second);
        ++fr.pins;
        return PageGuard(this, it->second, &fr.page);
    }
    ++stats_.misses;
    const std::size_t f = acquireFrame();
    Frame& fr = *frames_[f];
    try {
        store_.read(id, fr.page);
    } catch (...) {
        freeFrames_.push_back(f);
        throw;
    }
    fr.id = id;
    fr.pins = 1;
    fr.dirty = false;
    table_[id] = f;
    return PageGuard(this, f, &fr.page);
}

PageGuard BufferPool::newPage(PageId& outId) {
    const std::size_t f = acquireFrame();
    Frame& fr = *frames_[f];
    outId = store_.allocate();
    fr.page.init(outId);
    fr.id = outId;
    fr.pins = 1;
    fr.dirty = true;
    table_[outId] = f;
    return PageGuard(this, f, &fr.page);
}

void BufferPool::peek(PageId id, Page& out) {
    auto it = table_.find(id);
    if (it != table_.end()) {
        out = frames_[it->second]->page;
        return;
    }
    store_.read(id, out);
}

void BufferPool::unpin(std::size_t f) {
    Frame& fr = *frames_[f];
    if (fr.pins <= 0) throw std::logic_error("unpin of unpinned frame");
    if (--fr.pins == 0) {
        lru_.push_front(f);
        fr.lruPos = lru_.begin();
        fr.inLru = true;
    }
}

void BufferPool::flushAll() {
    for (auto& fr : frames_) {
        if (fr->id != kInvalidPageId && fr->dirty) {
            store_.write(fr->id, fr->page);
            ++stats_.writes;
            fr->dirty = false;
        }
    }
    store_.sync();
}

void BufferPool::evictAll() {
    for (auto& fr : frames_) {
        if (fr->pins > 0) throw std::logic_error("evictAll with pinned pages");
    }
    flushAll();
    table_.clear();
    lru_.clear();
    freeFrames_.clear();
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        frames_[i]->id = kInvalidPageId;
        frames_[i]->inLru = false;
        freeFrames_.push_back(frames_.size() - 1 - i);
    }
}

void BufferPool::resize(std::size_t capacity) {
    evictAll();
    if (capacity < 4) capacity = 4;
    frames_.clear();
    freeFrames_.clear();
    for (std::size_t i = 0; i < capacity; ++i) {
        frames_.push_back(std::make_unique<Frame>());
        freeFrames_.push_back(capacity - 1 - i);
    }
}

}  // namespace querylab::storage

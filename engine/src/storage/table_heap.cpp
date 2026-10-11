#include "querylab/storage/table_heap.h"

#include "querylab/common/error.h"

namespace querylab::storage {

Rid TableHeap::insert(const Row& row) {
    const auto bytes = codec::encodeRow(row);
    if (bytes.size() > Page::kMaxRowSize) {
        throw QueryError(ErrorStage::Storage, "row of " + std::to_string(bytes.size()) + " bytes exceeds the maximum row size of " +
                                                  std::to_string(Page::kMaxRowSize) + " bytes");
    }
    if (!pages_.empty()) {
        PageGuard g = pool_->fetch(pages_.back());
        if (auto slot = g.mutablePage().insert(bytes)) {
            ++rowCount_;
            return Rid{pages_.back(), *slot};
        }
    }
    PageId id;
    PageGuard g = pool_->newPage(id);
    auto slot = g.mutablePage().insert(bytes);
    if (!slot) throw std::logic_error("row does not fit in an empty page");
    pages_.push_back(id);
    ++rowCount_;
    return Rid{id, *slot};
}

Row TableHeap::fetch(const Rid& rid) const {
    PageGuard g = pool_->fetch(rid.page);
    return codec::decodeRow(g.page().get(rid.slot), columnCount_);
}

void TableHeap::fetchColumns(const Rid& rid, const std::vector<bool>& wanted, Row& out) const {
    PageGuard g = pool_->fetch(rid.page);
    codec::decodeColumns(g.page().get(rid.slot), wanted, out);
}

void TableHeap::erase(const Rid& rid) {
    PageGuard g = pool_->fetch(rid.page);
    g.mutablePage().erase(rid.slot);
    --rowCount_;
}

Rid TableHeap::update(const Rid& rid, const Row& row) {
    const auto bytes = codec::encodeRow(row);
    {
        PageGuard g = pool_->fetch(rid.page);
        if (g.mutablePage().updateInPlace(rid.slot, bytes)) return rid;
    }
    erase(rid);
    return insert(row);
}

void TableHeap::forEach(const std::function<void(const Rid&, const Row&)>& fn) const {
    for (PageId pid : pages_) {
        std::vector<std::pair<Rid, Row>> batch;
        {
            PageGuard g = pool_->fetch(pid);
            const Page& p = g.page();
            for (std::uint16_t s = 0; s < p.slotCount(); ++s) {
                if (p.isLive(s)) batch.emplace_back(Rid{pid, s}, codec::decodeRow(p.get(s), columnCount_));
            }
        }
        for (auto& [rid, row] : batch) fn(rid, row);
    }
}

}  // namespace querylab::storage

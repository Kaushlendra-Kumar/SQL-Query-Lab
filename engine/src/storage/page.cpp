#include "querylab/storage/page.h"

#include <bit>
#include <cstring>
#include <stdexcept>

#include "querylab/common/error.h"

namespace querylab::storage {

namespace codec {

namespace {
enum Tag : std::uint8_t { kNull = 0, kFalse = 1, kTrue = 2, kInt = 3, kFloat = 4, kText = 5 };

void putU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
std::uint64_t getU64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}
std::uint32_t getU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

[[noreturn]] void corrupt() { throw QueryError(ErrorStage::Storage, "corrupted row image"); }

/// Decodes (or skips) one value at `pos`, returning the new position.
std::size_t step(std::span<const std::uint8_t> b, std::size_t pos, Value* out) {
    if (pos >= b.size()) corrupt();
    const std::uint8_t tag = b[pos++];
    switch (tag) {
        case kNull:
            if (out) *out = Value::null();
            return pos;
        case kFalse:
        case kTrue:
            if (out) *out = Value::boolean(tag == kTrue);
            return pos;
        case kInt:
            if (pos + 8 > b.size()) corrupt();
            if (out) *out = Value::integer(static_cast<std::int64_t>(getU64(b.data() + pos)));
            return pos + 8;
        case kFloat:
            if (pos + 8 > b.size()) corrupt();
            if (out) *out = Value::floating(std::bit_cast<double>(getU64(b.data() + pos)));
            return pos + 8;
        case kText: {
            if (pos + 4 > b.size()) corrupt();
            const std::uint32_t len = getU32(b.data() + pos);
            pos += 4;
            if (pos + len > b.size()) corrupt();
            if (out) *out = Value::text(std::string(reinterpret_cast<const char*>(b.data() + pos), len));
            return pos + len;
        }
        default:
            corrupt();
    }
}
}  // namespace

std::vector<std::uint8_t> encodeRow(const Row& row) {
    std::vector<std::uint8_t> out;
    out.reserve(row.size() * 9);
    for (const auto& v : row) {
        switch (v.type()) {
            case DataType::Null: out.push_back(kNull); break;
            case DataType::Boolean: out.push_back(v.asBool() ? kTrue : kFalse); break;
            case DataType::Integer:
                out.push_back(kInt);
                putU64(out, static_cast<std::uint64_t>(v.asInt()));
                break;
            case DataType::Float:
                out.push_back(kFloat);
                putU64(out, std::bit_cast<std::uint64_t>(v.asFloat()));
                break;
            case DataType::Text: {
                out.push_back(kText);
                const auto& s = v.asText();
                const auto len = static_cast<std::uint32_t>(s.size());
                for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(len >> (8 * i)));
                out.insert(out.end(), s.begin(), s.end());
                break;
            }
        }
    }
    return out;
}

Row decodeRow(std::span<const std::uint8_t> bytes, std::size_t columnCount) {
    Row row(columnCount);
    std::size_t pos = 0;
    for (std::size_t i = 0; i < columnCount; ++i) pos = step(bytes, pos, &row[i]);
    return row;
}

void decodeColumns(std::span<const std::uint8_t> bytes, const std::vector<bool>& wanted, Row& out, bool reset) {
    if (reset || out.size() != wanted.size()) out.assign(wanted.size(), Value());
    std::size_t pos = 0;
    std::size_t last = 0;
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (wanted[i]) last = i + 1;
    }
    for (std::size_t i = 0; i < last; ++i) pos = step(bytes, pos, wanted[i] ? &out[i] : nullptr);
}

}  // namespace codec

std::uint16_t Page::u16(std::size_t off) const {
    return static_cast<std::uint16_t>(data_[off] | (data_[off + 1] << 8));
}
void Page::setU16(std::size_t off, std::uint16_t v) {
    data_[off] = static_cast<std::uint8_t>(v);
    data_[off + 1] = static_cast<std::uint8_t>(v >> 8);
}
std::uint32_t Page::u32(std::size_t off) const {
    return static_cast<std::uint32_t>(u16(off)) | static_cast<std::uint32_t>(u16(off + 2)) << 16;
}
void Page::setU32(std::size_t off, std::uint32_t v) {
    setU16(off, static_cast<std::uint16_t>(v));
    setU16(off + 2, static_cast<std::uint16_t>(v >> 16));
}

void Page::init(PageId id) {
    data_.fill(0);
    setU32(0, id);
    setU16(8, 0);
    setU16(10, static_cast<std::uint16_t>(kPageSize));  // 4096 fits in u16
}

PageId Page::id() const { return u32(0); }
std::uint16_t Page::slotCount() const { return u16(8); }

std::size_t Page::freeSpace() const {
    const std::size_t dirEnd = kHeaderSize + slotCount() * kSlotSize;
    const std::size_t dataStart = u16(10) == 0 ? kPageSize : u16(10);
    return dataStart > dirEnd ? dataStart - dirEnd : 0;
}

bool Page::isLive(std::uint16_t slot) const {
    return slot < slotCount() && u16(kHeaderSize + slot * kSlotSize + 2) != 0;
}

std::size_t Page::liveCount() const {
    std::size_t n = 0;
    for (std::uint16_t s = 0; s < slotCount(); ++s) n += isLive(s);
    return n;
}

std::optional<std::uint16_t> Page::insert(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxRowSize) return std::nullopt;
    if (freeSpace() < bytes.size() + kSlotSize) return std::nullopt;
    if (slotCount() == 0xFFFF) return std::nullopt;
    const std::size_t dataStart = u16(10) == 0 ? kPageSize : u16(10);
    const std::size_t off = dataStart - bytes.size();
    std::memcpy(data_.data() + off, bytes.data(), bytes.size());
    const std::uint16_t slot = slotCount();
    setU16(kHeaderSize + slot * kSlotSize, static_cast<std::uint16_t>(off));
    setU16(kHeaderSize + slot * kSlotSize + 2, static_cast<std::uint16_t>(bytes.size()));
    setU16(8, static_cast<std::uint16_t>(slot + 1));
    setU16(10, static_cast<std::uint16_t>(off));
    return slot;
}

std::span<const std::uint8_t> Page::get(std::uint16_t slot) const {
    if (!isLive(slot)) throw QueryError(ErrorStage::Storage, "row " + std::to_string(slot) + " on page " + std::to_string(id()) + " does not exist");
    const std::size_t off = u16(kHeaderSize + slot * kSlotSize);
    const std::size_t len = u16(kHeaderSize + slot * kSlotSize + 2);
    if (off + len > kPageSize || off < kHeaderSize) throw QueryError(ErrorStage::Storage, "corrupted slot directory on page " + std::to_string(id()));
    return {data_.data() + off, len};
}

void Page::erase(std::uint16_t slot) {
    if (!isLive(slot)) throw std::logic_error("erase of dead slot");
    // Tombstone; space is not compacted (documented simplification).
    setU16(kHeaderSize + slot * kSlotSize + 2, 0);
}

bool Page::updateInPlace(std::uint16_t slot, std::span<const std::uint8_t> bytes) {
    if (!isLive(slot) || bytes.empty()) return false;
    const std::size_t off = u16(kHeaderSize + slot * kSlotSize);
    const std::size_t len = u16(kHeaderSize + slot * kSlotSize + 2);
    if (bytes.size() > len) return false;
    std::memcpy(data_.data() + off, bytes.data(), bytes.size());
    setU16(kHeaderSize + slot * kSlotSize + 2, static_cast<std::uint16_t>(bytes.size()));
    return true;
}

std::uint32_t Page::computeChecksum() const {
    // FNV-1a over the page with the checksum field treated as zero.
    std::uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < kPageSize; ++i) {
        const std::uint8_t b = (i >= 4 && i < 8) ? 0 : data_[i];
        h = (h ^ b) * 16777619u;
    }
    return h == 0 ? 1 : h;
}

void Page::stampChecksum() { setU32(4, computeChecksum()); }
bool Page::verifyChecksum() const { return u32(4) == computeChecksum(); }

}  // namespace querylab::storage

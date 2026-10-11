#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "querylab/common/value.h"

namespace querylab::storage {

constexpr std::size_t kPageSize = 4096;
using PageId = std::uint32_t;
constexpr PageId kInvalidPageId = 0xFFFFFFFFu;

/// Stable row identifier: (page, slot). Slots are never reused within a page,
/// so a RID stays valid until its row is deleted.
struct Rid {
    PageId page = kInvalidPageId;
    std::uint16_t slot = 0;
    friend bool operator==(const Rid&, const Rid&) = default;
    friend auto operator<=>(const Rid&, const Rid&) = default;
    [[nodiscard]] std::string toString() const { return "(" + std::to_string(page) + "," + std::to_string(slot) + ")"; }
};

/// Row codec. Layout per value: 1 tag byte, then payload
///   NULL: none | BOOL: 1 byte | INTEGER: 8 bytes LE | FLOAT: 8 bytes IEEE LE |
///   TEXT: 4-byte LE length + bytes.
namespace codec {
std::vector<std::uint8_t> encodeRow(const Row& row);
/// Decodes all columns.
Row decodeRow(std::span<const std::uint8_t> bytes, std::size_t columnCount);
/// Decodes only the columns whose `wanted[i]` is true; others are left NULL
/// (or, with reset=false, untouched). Skipping avoids allocating strings for
/// columns the query does not need.
void decodeColumns(std::span<const std::uint8_t> bytes, const std::vector<bool>& wanted, Row& out, bool reset = true);
}  // namespace codec

/// Fixed-size slotted page.
///
///  [0..4)   page id            [4..8)   checksum (file mode)
///  [8..10)  slot count         [10..12) free-space end (start of tuple data)
///  [12..16) reserved
///  [16..)   slot directory: {u16 offset, u16 length} per slot, growing up
///  tuple data grows down from the end of the page.
/// A slot with length 0 is a tombstone (deleted row).
class Page {
public:
    static constexpr std::size_t kHeaderSize = 16;
    static constexpr std::size_t kSlotSize = 4;
    /// Largest row that fits in an empty page.
    static constexpr std::size_t kMaxRowSize = kPageSize - kHeaderSize - kSlotSize;

    Page() { data_.fill(0); }
    void init(PageId id);

    [[nodiscard]] PageId id() const;
    [[nodiscard]] std::uint16_t slotCount() const;
    [[nodiscard]] std::size_t freeSpace() const;
    [[nodiscard]] bool isLive(std::uint16_t slot) const;
    [[nodiscard]] std::size_t liveCount() const;

    /// Returns the new slot, or nullopt if the row does not fit.
    std::optional<std::uint16_t> insert(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::span<const std::uint8_t> get(std::uint16_t slot) const;
    void erase(std::uint16_t slot);
    /// In-place update; succeeds only if the new image is not larger than the old.
    bool updateInPlace(std::uint16_t slot, std::span<const std::uint8_t> bytes);

    [[nodiscard]] std::uint32_t computeChecksum() const;
    void stampChecksum();
    [[nodiscard]] bool verifyChecksum() const;

    std::uint8_t* raw() { return data_.data(); }
    [[nodiscard]] const std::uint8_t* raw() const { return data_.data(); }

private:
    std::uint16_t u16(std::size_t off) const;
    void setU16(std::size_t off, std::uint16_t v);
    std::uint32_t u32(std::size_t off) const;
    void setU32(std::size_t off, std::uint32_t v);

    std::array<std::uint8_t, kPageSize> data_;
};

}  // namespace querylab::storage

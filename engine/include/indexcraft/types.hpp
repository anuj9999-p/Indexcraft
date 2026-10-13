#pragma once
// Core value types shared by the heap file, the B+ Tree and the query layer.

#include <cstdint>
#include <string>

namespace indexcraft {

using Key = std::int64_t;          // unique indexed key
using RecordId = std::uint64_t;    // stable record identifier; assigned once, never reused

enum class RecordStatus : std::uint8_t { Deleted = 0, Live = 1 };

// Where a record lives. The B+ Tree stores this as the value for a key.
// `rid` is the stable identity; `offset` is the byte offset of the slot in the
// heap file. A slot's offset can be reused after a delete, so every read through
// a locator re-checks that the slot's rid still equals locator.rid.
struct RecordLocator {
  RecordId rid = 0;
  std::uint64_t offset = 0;
};

struct Record {
  Key key = 0;
  RecordId rid = 0;
  std::string payload;  // arbitrary bytes, up to kMaxPayloadBytes
};

}  // namespace indexcraft

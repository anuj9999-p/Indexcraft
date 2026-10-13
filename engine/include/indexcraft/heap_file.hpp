#pragma once
// HeapFile: persistent, append-oriented record store in a single binary file.
//
// ON-DISK FORMAT (all integers little-endian, version 1)
//
//   File header, 64 bytes:
//     0..7    magic "INDXCRFT"
//     8..11   format version (u32) = 1
//     12..15  file header size (u32) = 64
//     16..19  slot header size (u32) = 36
//     20..23  flags (u32) = 0
//     24..31  creation time, unix seconds (u64)
//     32..59  reserved, zero
//     60..63  CRC-32 of bytes 0..59
//
//   Then zero or more record slots, back to back until end of file:
//     0..3    slot magic "REC1"
//     4       status (u8): 1 = live, 0 = deleted (tombstone)
//     5..7    reserved, must be zero
//     8..15   record id (u64)
//     16..23  key (i64)
//     24..27  capacity (u32): size of the payload area in bytes
//     28..31  length (u32): bytes of payload in use, length <= capacity
//     32..35  CRC-32 over bytes 8..31 of the slot header followed by payload[0..length)
//     36..    payload area, `capacity` bytes (bytes beyond `length` are zero)
//   A slot occupies 36 + capacity bytes, so a reader can always skip to the next slot.
//
// DELETE writes status=0 into the slot in place. The slot stays in the file and its
// capacity goes on an in-memory free list. A later insert whose payload fits (and is
// not wastefully small for the slot) rewrites the slot with a NEW record id and a new
// CRC. Otherwise the record is appended. Record ids are never reused. Free slots are
// not coalesced and the file is never compacted (see docs/limitations).
//
// On open the whole file is validated: any bad magic, truncated slot, impossible
// length, nonzero reserved bytes or CRC mismatch makes open() fail with Code::Corrupt
// and an offset in the message. Corrupt data is never silently skipped.
//
// NOT crash safe: there is no write-ahead log. A crash during a write can leave a
// torn slot, which the next open() reports as corruption.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include "indexcraft/status.hpp"
#include "indexcraft/types.hpp"

namespace indexcraft {

constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kFileHeaderSize = 64;
constexpr std::uint32_t kSlotHeaderSize = 36;
constexpr std::uint32_t kSlotMagic = 0x31434552u;  // bytes 'R','E','C','1' on disk
constexpr std::uint32_t kMaxPayloadBytes = 1u << 20;  // 1 MiB
extern const char kFileMagic[8];                      // "INDXCRFT"

struct ScanStats {
  std::uint64_t slots_seen = 0;     // live + deleted slots walked
  std::uint64_t live_records = 0;   // live records visited
  std::uint64_t bytes_read = 0;     // bytes read from the file by this scan
};

class HeapFile {
 public:
  // Called for every live record while open() validates the file. Used by Database
  // to rebuild the B+ Tree in the same pass. A non-ok Status aborts open().
  using LiveVisitor = std::function<Status(Key, const RecordLocator&)>;
  // Zero-copy scan callback. Return false to stop the scan early.
  using RawVisitor = std::function<bool(Key key, RecordId rid, const char* payload,
                                        std::uint32_t length, std::uint64_t offset)>;
  using RecordVisitor = std::function<bool(const Record&, std::uint64_t offset)>;

  HeapFile() = default;
  ~HeapFile();
  HeapFile(const HeapFile&) = delete;
  HeapFile& operator=(const HeapFile&) = delete;

  Status open(const std::string& path, bool create_if_missing, const LiveVisitor& on_live = nullptr);
  void close();
  bool is_open() const { return fd_ >= 0; }
  Status sync();

  Result<RecordLocator> insert(Key key, const std::string& payload);
  Result<Record> get(RecordId rid) const;
  Result<Record> read_at(const RecordLocator& loc) const;
  Status erase(RecordId rid);

  Status scan(const RecordVisitor& visitor, ScanStats* stats = nullptr) const;
  Status scan_raw(const RawVisitor& visitor, ScanStats* stats = nullptr) const;

  const std::string& path() const { return path_; }
  std::uint64_t live_count() const { return live_count_; }
  std::uint64_t deleted_slots() const { return deleted_slots_; }
  std::uint64_t file_size() const { return file_size_; }
  RecordId next_rid() const { return next_rid_; }
  bool contains_rid(RecordId rid) const { return rid_to_offset_.count(rid) != 0; }
  std::optional<std::uint64_t> offset_of(RecordId rid) const {
    auto it = rid_to_offset_.find(rid);
    if (it == rid_to_offset_.end()) return std::nullopt;
    return it->second;
  }
  // Cumulative number of pread calls / bytes used by point reads (read_at/get).
  std::uint64_t read_calls() const { return read_calls_; }
  std::uint64_t read_bytes() const { return read_bytes_; }

 private:
  struct SlotView {
    std::uint64_t offset;
    RecordStatus status;
    RecordId rid;
    Key key;
    std::uint32_t capacity;
    std::uint32_t length;
    const std::uint8_t* payload;
  };
  using SlotCallback = std::function<bool(const SlotView&, Status&)>;

  Status walk(const SlotCallback& cb, std::uint64_t* bytes_read) const;
  Status read_exact(void* buf, std::size_t n, std::uint64_t offset) const;
  Status write_exact(const void* buf, std::size_t n, std::uint64_t offset);
  void reset_state();

  int fd_ = -1;
  std::string path_;
  std::uint64_t file_size_ = 0;
  std::uint64_t live_count_ = 0;
  std::uint64_t deleted_slots_ = 0;
  RecordId next_rid_ = 1;
  std::unordered_map<RecordId, std::uint64_t> rid_to_offset_;  // live records only
  std::multimap<std::uint32_t, std::uint64_t> free_slots_;     // capacity -> offset
  mutable std::uint64_t read_calls_ = 0;
  mutable std::uint64_t read_bytes_ = 0;
};

}  // namespace indexcraft

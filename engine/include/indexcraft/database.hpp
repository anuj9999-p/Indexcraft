#pragma once
// Database: couples the persistent HeapFile with the in-memory BPlusTree index so the
// two stay logically consistent.
//
//   insert:  duplicate probe in the index -> append/reuse a heap slot -> insert into index
//   erase:   index lookup -> tombstone the heap slot -> remove from index
//   open:    validate the heap file and rebuild the index from its LIVE records
//
// Persistence: only the heap file is persistent. The B+ Tree is rebuilt on every open().
// There are no transactions or crash recovery; see docs for limitations.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "indexcraft/bplus_tree.hpp"
#include "indexcraft/heap_file.hpp"
#include "indexcraft/status.hpp"
#include "indexcraft/types.hpp"

namespace indexcraft {

struct DatabaseOptions {
  std::size_t max_keys = 64;     // B+ Tree node capacity (>= 3)
  bool create_if_missing = true;
};

struct DatabaseStats {
  std::uint64_t record_count = 0;
  std::uint64_t file_size_bytes = 0;
  std::uint64_t deleted_slots = 0;
  std::uint64_t next_rid = 0;
  std::size_t index_height = 0;
  std::size_t index_key_count = 0;
  std::size_t index_node_count = 0;
  std::size_t index_leaf_count = 0;
  std::size_t index_internal_count = 0;
  std::size_t index_max_keys = 0;
  std::uint64_t open_ns = 0;  // time to validate the file and rebuild the index at open()
};

struct ConsistencyReport {
  bool consistent = true;
  std::uint64_t heap_live_records = 0;
  std::uint64_t index_entries = 0;
  std::uint64_t records_checked = 0;
  std::vector<std::string> problems;  // capped at 50 entries
};

class Database {
 public:
  Database() = default;
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  Status open(const std::string& path, const DatabaseOptions& options = {});
  void close();
  bool is_open() const { return heap_.is_open(); }
  Status sync() { return heap_.sync(); }

  Status insert(Key key, const std::string& payload, RecordLocator* out = nullptr);
  Result<Record> get(Key key) const;
  Status erase(Key key);

  // Records in ascending key order, skipping `offset`, returning at most `limit`.
  Result<std::vector<Record>> list(std::size_t offset, std::size_t limit) const;

  // Discards the in-memory index and rebuilds it from the live heap records.
  Status rebuild_index();
  // Cross-checks every heap record against the index and vice versa (O(n)).
  ConsistencyReport check_consistency() const;

  DatabaseStats stats() const;
  const HeapFile& heap() const { return heap_; }
  const BPlusTree& index() const { return *index_; }

 private:
  HeapFile heap_;
  std::unique_ptr<BPlusTree> index_ = std::make_unique<BPlusTree>(64);
  std::size_t max_keys_ = 64;
  std::uint64_t open_ns_ = 0;
};

}  // namespace indexcraft

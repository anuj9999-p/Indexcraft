#include "indexcraft/database.hpp"

#include <chrono>

namespace indexcraft {

namespace {
Status duplicate_live_key(Key key) {
  return Status::Error(Code::Corrupt, "data file contains two live records with key " + std::to_string(key));
}
}  // namespace

Status Database::open(const std::string& path, const DatabaseOptions& options) {
  if (is_open()) return Status::Error(Code::InvalidArgument, "database is already open");
  if (options.max_keys < 3) return Status::Error(Code::InvalidArgument, "max_keys must be at least 3");
  max_keys_ = options.max_keys;
  index_ = std::make_unique<BPlusTree>(max_keys_);

  auto t0 = std::chrono::steady_clock::now();
  Status st = heap_.open(path, options.create_if_missing, [&](Key key, const RecordLocator& loc) {
    if (!index_->insert(key, loc)) return duplicate_live_key(key);
    return Status::Ok();
  });
  open_ns_ = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  if (!st.ok()) {
    index_ = std::make_unique<BPlusTree>(max_keys_);
    return st;
  }
  return Status::Ok();
}

void Database::close() {
  heap_.close();
  index_ = std::make_unique<BPlusTree>(max_keys_);
}

Status Database::insert(Key key, const std::string& payload, RecordLocator* out) {
  if (!is_open()) return Status::Error(Code::InvalidArgument, "database is not open");
  if (index_->find(key)) {
    return Status::Error(Code::DuplicateKey, "key " + std::to_string(key) + " already exists");
  }
  Result<RecordLocator> r = heap_.insert(key, payload);
  if (!r.ok()) return r.status();
  if (!index_->insert(key, r.value())) {
    (void)heap_.erase(r.value().rid);  // roll back the heap write
    return Status::Error(Code::Internal, "index rejected a key that was absent a moment ago");
  }
  if (out) *out = r.value();
  return Status::Ok();
}

Result<Record> Database::get(Key key) const {
  if (!is_open()) return Status::Error(Code::InvalidArgument, "database is not open");
  auto loc = index_->find(key);
  if (!loc) return Status::Error(Code::NotFound, "no record with key " + std::to_string(key));
  Result<Record> r = heap_.read_at(*loc);
  if (!r.ok()) return r;
  if (r.value().key != key) {
    return Status::Error(Code::Corrupt, "heap record key " + std::to_string(r.value().key) +
                                            " does not match index key " + std::to_string(key));
  }
  return r;
}

Status Database::erase(Key key) {
  if (!is_open()) return Status::Error(Code::InvalidArgument, "database is not open");
  auto loc = index_->find(key);
  if (!loc) return Status::Error(Code::NotFound, "no record with key " + std::to_string(key));
  Status st = heap_.erase(loc->rid);
  if (!st.ok()) return st;
  if (!index_->erase(key)) return Status::Error(Code::Internal, "index lost key during erase");
  return Status::Ok();
}

Result<std::vector<Record>> Database::list(std::size_t offset, std::size_t limit) const {
  if (!is_open()) return Status::Error(Code::InvalidArgument, "database is not open");
  std::vector<Record> out;
  if (limit == 0) return out;
  Status err;
  index_->for_each(offset, [&](Key, const RecordLocator& loc) {
    Result<Record> r = heap_.read_at(loc);
    if (!r.ok()) {
      err = r.status();
      return false;
    }
    out.push_back(r.take());
    return out.size() < limit;
  });
  if (!err.ok()) return err;
  return out;
}

Status Database::rebuild_index() {
  if (!is_open()) return Status::Error(Code::InvalidArgument, "database is not open");
  index_ = std::make_unique<BPlusTree>(max_keys_);
  Status dup;
  Status st = heap_.scan_raw([&](Key key, RecordId rid, const char*, std::uint32_t, std::uint64_t offset) {
    if (!index_->insert(key, RecordLocator{rid, offset})) {
      dup = duplicate_live_key(key);
      return false;
    }
    return true;
  });
  if (!dup.ok()) return dup;
  return st;
}

ConsistencyReport Database::check_consistency() const {
  ConsistencyReport rep;
  auto problem = [&](const std::string& msg) {
    rep.consistent = false;
    if (rep.problems.size() < 50) rep.problems.push_back(msg);
  };
  if (!is_open()) {
    problem("database is not open");
    return rep;
  }
  rep.heap_live_records = heap_.live_count();
  rep.index_entries = index_->size();
  if (rep.heap_live_records != rep.index_entries) {
    problem("heap has " + std::to_string(rep.heap_live_records) + " live records but the index has " +
            std::to_string(rep.index_entries) + " entries");
  }
  // Direction 1: every live record on disk must be indexed with the same locator.
  Status st = heap_.scan_raw([&](Key key, RecordId rid, const char*, std::uint32_t, std::uint64_t offset) {
    ++rep.records_checked;
    auto loc = index_->find(key);
    if (!loc) problem("record " + std::to_string(rid) + " (key " + std::to_string(key) + ") is missing from the index");
    else if (loc->rid != rid || loc->offset != offset) problem("index locator for key " + std::to_string(key) + " does not match the heap slot");
    return true;
  });
  if (!st.ok()) problem("heap scan failed: " + st.to_string());
  // Direction 2: every index entry must point at a live heap record with the same offset.
  index_->for_each(0, [&](Key key, const RecordLocator& loc) {
    auto off = heap_.offset_of(loc.rid);
    if (!off) problem("index key " + std::to_string(key) + " points at record " + std::to_string(loc.rid) + " which is not live");
    else if (*off != loc.offset) problem("index key " + std::to_string(key) + " has a stale heap offset");
    return true;
  });
  for (const auto& v : index_->validate()) problem("B+ Tree: " + v);
  return rep;
}

DatabaseStats Database::stats() const {
  DatabaseStats s;
  s.record_count = heap_.live_count();
  s.file_size_bytes = heap_.file_size();
  s.deleted_slots = heap_.deleted_slots();
  s.next_rid = heap_.next_rid();
  s.index_height = index_->height();
  s.index_key_count = index_->size();
  s.index_node_count = index_->node_count();
  s.index_leaf_count = index_->leaf_count();
  s.index_internal_count = index_->internal_count();
  s.index_max_keys = index_->max_keys();
  s.open_ns = open_ns_;
  return s;
}

}  // namespace indexcraft

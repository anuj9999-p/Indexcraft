#include <map>
#include <random>

#include "indexcraft/database.hpp"
#include "test_util.hpp"

using namespace indexcraft;
using namespace testutil;

namespace {

DatabaseOptions opts(std::size_t m = 4) {
  DatabaseOptions o;
  o.max_keys = m;
  return o;
}

std::map<Key, std::string> dump(const Database& db) {
  std::map<Key, std::string> out;
  auto r = db.list(0, db.stats().record_count + 1);
  EXPECT_TRUE(r.ok());
  if (r.ok()) for (auto& rec : r.value()) out[rec.key] = rec.payload;
  return out;
}

}  // namespace

TEST(Database, EmptyDatabase) {
  TempDir dir;
  Database db;
  ASSERT_TRUE(db.open(dir.file("db.dat"), opts()).ok());
  auto s = db.stats();
  EXPECT_EQ(s.record_count, 0u);
  EXPECT_EQ(s.index_key_count, 0u);
  EXPECT_EQ(s.index_height, 1u);
  EXPECT_EQ(db.get(1).status().code(), Code::NotFound);
  EXPECT_EQ(db.erase(1).code(), Code::NotFound);
  auto l = db.list(0, 10);
  ASSERT_TRUE(l.ok());
  EXPECT_TRUE(l.value().empty());
  EXPECT_TRUE(db.check_consistency().consistent);
}

TEST(Database, InsertAndRetrieve) {
  TempDir dir;
  Database db;
  ASSERT_TRUE(db.open(dir.file("db.dat"), opts()).ok());
  RecordLocator loc;
  ASSERT_TRUE(db.insert(7, "seven", &loc).ok());
  ASSERT_TRUE(db.insert(3, "three").ok());
  auto r = db.get(7);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().key, 7);
  EXPECT_EQ(r.value().payload, "seven");
  EXPECT_EQ(r.value().rid, loc.rid);
  EXPECT_EQ(db.stats().record_count, 2u);
}

TEST(Database, SequentialScanReturnsInsertionOrderOfLiveRecords) {
  TempDir dir;
  Database db;
  ASSERT_TRUE(db.open(dir.file("db.dat"), opts()).ok());
  for (Key k : {30, 10, 20, 50, 40}) ASSERT_TRUE(db.insert(k, "p" + std::to_string(k)).ok());
  ASSERT_TRUE(db.erase(20).ok());
  std::vector<Key> keys;
  ASSERT_TRUE(db.heap().scan([&](const Record& r, std::uint64_t) { keys.push_back(r.key); return true; }).ok());
  EXPECT_EQ(keys, (std::vector<Key>{30, 10, 50, 40}));  // heap order, not key order
  auto sorted = db.list(0, 10);
  ASSERT_TRUE(sorted.ok());
  std::vector<Key> by_key;
  for (auto& r : sorted.value()) by_key.push_back(r.key);
  EXPECT_EQ(by_key, (std::vector<Key>{10, 30, 40, 50}));  // index order is sorted
}

TEST(Database, DeleteThenSearch) {
  TempDir dir;
  Database db;
  ASSERT_TRUE(db.open(dir.file("db.dat"), opts()).ok());
  for (Key k = 1; k <= 20; ++k) ASSERT_TRUE(db.insert(k, "v").ok());
  ASSERT_TRUE(db.erase(10).ok());
  EXPECT_EQ(db.get(10).status().code(), Code::NotFound);
  EXPECT_TRUE(db.get(9).ok());
  EXPECT_TRUE(db.get(11).ok());
  EXPECT_EQ(db.stats().record_count, 19u);
  EXPECT_EQ(db.stats().deleted_slots, 1u);
  EXPECT_TRUE(db.check_consistency().consistent);
  ASSERT_TRUE(db.insert(10, "again").ok());  // the key can be inserted again after deletion
  EXPECT_EQ(db.get(10).value().payload, "again");
}

TEST(Database, DuplicateKeyRejectedWithoutTouchingTheFile) {
  TempDir dir;
  Database db;
  ASSERT_TRUE(db.open(dir.file("db.dat"), opts()).ok());
  ASSERT_TRUE(db.insert(5, "first").ok());
  const auto size = db.stats().file_size_bytes;
  Status st = db.insert(5, "second");
  EXPECT_EQ(st.code(), Code::DuplicateKey);
  EXPECT_NE(st.message().find("5"), std::string::npos);
  EXPECT_EQ(db.stats().file_size_bytes, size);
  EXPECT_EQ(db.stats().record_count, 1u);
  EXPECT_EQ(db.get(5).value().payload, "first");
}

TEST(Database, MissingKeyErrors) {
  TempDir dir;
  Database db;
  ASSERT_TRUE(db.open(dir.file("db.dat"), opts()).ok());
  db.insert(1, "x");
  EXPECT_EQ(db.get(2).status().code(), Code::NotFound);
  EXPECT_EQ(db.erase(2).code(), Code::NotFound);
  EXPECT_EQ(db.stats().record_count, 1u);
}

TEST(Database, OperationsOnClosedDatabaseFailCleanly) {
  Database db;
  EXPECT_EQ(db.insert(1, "x").code(), Code::InvalidArgument);
  EXPECT_EQ(db.get(1).status().code(), Code::InvalidArgument);
  EXPECT_EQ(db.erase(1).code(), Code::InvalidArgument);
  EXPECT_FALSE(db.check_consistency().consistent);
}

TEST(Database, InvalidNodeCapacityRejected) {
  TempDir dir;
  Database db;
  DatabaseOptions o;
  o.max_keys = 2;
  EXPECT_EQ(db.open(dir.file("db.dat"), o).code(), Code::InvalidArgument);
}

TEST(Database, PersistenceAfterRestart) {
  TempDir dir;
  const std::string path = dir.file("db.dat");
  std::map<Key, std::string> ref;
  std::mt19937_64 rng(11);
  {
    Database db;
    ASSERT_TRUE(db.open(path, opts()).ok());
    for (int i = 0; i < 1500; ++i) {
      Key k = static_cast<Key>(rng() % 4000) - 2000;
      std::string p = random_payload(rng, 200);
      if (db.insert(k, p).ok()) ref[k] = p;
    }
    int removed = 0;
    for (auto it = ref.begin(); it != ref.end() && removed < 400;) {
      if (rng() % 3 == 0) { ASSERT_TRUE(db.erase(it->first).ok()); it = ref.erase(it); ++removed; } else ++it;
    }
    EXPECT_EQ(dump(db), ref);
  }
  Database db;
  ASSERT_TRUE(db.open(path, opts()).ok());  // index is rebuilt from live heap records
  EXPECT_EQ(db.stats().record_count, ref.size());
  EXPECT_EQ(dump(db), ref);
  for (auto& [k, v] : ref) ASSERT_EQ(db.get(k).value().payload, v);
  EXPECT_TRUE(db.index().validate().empty());
  EXPECT_TRUE(db.check_consistency().consistent);
  // deleted keys stay deleted
  int checked = 0;
  for (Key k = -2000; k < 2000 && checked < 50; ++k) {
    if (!ref.count(k)) { EXPECT_EQ(db.get(k).status().code(), Code::NotFound); ++checked; }
  }
  // and the reopened database is fully writable
  ASSERT_TRUE(db.insert(100000, "after restart").ok());
}

TEST(Database, ReopenWithDifferentNodeCapacityGivesSameLogicalContent) {
  TempDir dir;
  const std::string path = dir.file("db.dat");
  std::map<Key, std::string> ref;
  {
    Database db;
    ASSERT_TRUE(db.open(path, opts(3)).ok());
    for (Key k = 0; k < 300; ++k) { db.insert(k * 3, "v" + std::to_string(k)); ref[k * 3] = "v" + std::to_string(k); }
  }
  Database db;
  ASSERT_TRUE(db.open(path, opts(64)).ok());
  EXPECT_EQ(dump(db), ref);
  EXPECT_EQ(db.stats().index_max_keys, 64u);
}

TEST(Database, IndexRebuildMatchesLiveHeap) {
  TempDir dir;
  Database db;
  ASSERT_TRUE(db.open(dir.file("db.dat"), opts()).ok());
  std::mt19937_64 rng(2);
  for (int i = 0; i < 800; ++i) db.insert(static_cast<Key>(rng() % 1000), random_payload(rng));
  for (Key k = 0; k < 1000; k += 7) db.erase(k);
  auto before = dump(db);
  const auto stats_before = db.stats();
  ASSERT_TRUE(db.rebuild_index().ok());
  EXPECT_EQ(dump(db), before);
  EXPECT_EQ(db.stats().index_key_count, stats_before.index_key_count);
  EXPECT_TRUE(db.check_consistency().consistent);
  EXPECT_TRUE(db.index().validate().empty());
}

TEST(Database, OpenFailsWhenFileHasTwoLiveRecordsWithTheSameKey) {
  TempDir dir;
  {
    HeapFile h;
    ASSERT_TRUE(h.open(dir.file("db.dat"), true).ok());
    h.insert(9, "a");
    h.insert(9, "b");
  }
  Database db;
  Status st = db.open(dir.file("db.dat"), opts());
  EXPECT_EQ(st.code(), Code::Corrupt);
  EXPECT_NE(st.message().find("two live records with key 9"), std::string::npos);
  EXPECT_FALSE(db.is_open());
}

TEST(Database, OpenFailsOnCorruptHeapFile) {
  TempDir dir;
  {
    Database db;
    ASSERT_TRUE(db.open(dir.file("db.dat"), opts()).ok());
    db.insert(1, "hello world");
  }
  auto bytes = read_file(dir.file("db.dat"));
  bytes[bytes.size() - 2] ^= 0xFF;
  write_file(dir.file("db.dat"), bytes);
  Database db;
  EXPECT_EQ(db.open(dir.file("db.dat"), opts()).code(), Code::Corrupt);
}

TEST(Database, ConsistencyCheckerDetectsAHeapRecordMissingFromTheIndex) {
  TempDir dir;
  Database db;
  ASSERT_TRUE(db.open(dir.file("db.dat"), opts()).ok());
  db.insert(1, "indexed");
  // Test-only sabotage: write straight into the heap, bypassing the index.
  ASSERT_TRUE(const_cast<HeapFile&>(db.heap()).insert(2, "orphan").ok());
  ConsistencyReport rep = db.check_consistency();
  EXPECT_FALSE(rep.consistent);
  EXPECT_FALSE(rep.problems.empty());
  ASSERT_TRUE(db.rebuild_index().ok());  // rebuilding from the heap repairs the index
  EXPECT_TRUE(db.check_consistency().consistent);
  EXPECT_TRUE(db.get(2).ok());
}

TEST(Database, ListPaginationIsStable) {
  TempDir dir;
  Database db;
  ASSERT_TRUE(db.open(dir.file("db.dat"), opts()).ok());
  for (Key k = 1; k <= 95; ++k) db.insert(k, "p");
  std::vector<Key> seen;
  for (std::size_t off = 0; off < 100; off += 25) {
    auto page = db.list(off, 25);
    ASSERT_TRUE(page.ok());
    for (auto& r : page.value()) seen.push_back(r.key);
  }
  ASSERT_EQ(seen.size(), 95u);
  for (std::size_t i = 0; i < seen.size(); ++i) EXPECT_EQ(seen[i], static_cast<Key>(i + 1));
}

TEST(DatabaseReferenceModel, RandomOperationsWithPeriodicRestartsMatchStdMap) {
  TempDir dir;
  const std::string path = dir.file("db.dat");
  auto db = std::make_unique<Database>();
  ASSERT_TRUE(db->open(path, opts(5)).ok());
  std::map<Key, std::string> ref;
  std::mt19937_64 rng(77);
  for (int i = 1; i <= 20000; ++i) {
    Key k = static_cast<Key>(rng() % 600) - 300;
    switch (rng() % 3) {
      case 0: {
        std::string p = random_payload(rng, 150);
        Status st = db->insert(k, p);
        ASSERT_EQ(st.ok(), ref.find(k) == ref.end()) << st.to_string();
        if (st.ok()) { ref[k] = p; } else { ASSERT_EQ(st.code(), Code::DuplicateKey); }
        break;
      }
      case 1: {
        Status st = db->erase(k);
        ASSERT_EQ(st.ok(), ref.erase(k) == 1);
        if (!st.ok()) { ASSERT_EQ(st.code(), Code::NotFound); }
        break;
      }
      default: {
        auto r = db->get(k);
        auto it = ref.find(k);
        ASSERT_EQ(r.ok(), it != ref.end());
        if (r.ok()) { ASSERT_EQ(r.value().payload, it->second); }
      }
    }
    if (i % 2500 == 0) {
      ASSERT_EQ(dump(*db), ref);
      ASSERT_TRUE(db->check_consistency().consistent);
      db = std::make_unique<Database>();  // simulated restart
      ASSERT_TRUE(db->open(path, opts(5)).ok());
      ASSERT_EQ(dump(*db), ref);
    }
  }
}

TEST(DatabaseLarge, TwoHundredThousandRecords) {
  TempDir dir;
  Database db;
  DatabaseOptions o;
  o.max_keys = 64;
  ASSERT_TRUE(db.open(dir.file("db.dat"), o).ok());
  const Key n = 200000;
  std::vector<Key> keys(n);
  for (Key i = 0; i < n; ++i) keys[i] = i * 3 + 1;
  std::mt19937_64 rng(1);
  for (std::size_t i = keys.size() - 1; i > 0; --i) std::swap(keys[i], keys[rng() % (i + 1)]);
  for (Key k : keys) ASSERT_TRUE(db.insert(k, "payload-" + std::to_string(k)).ok());
  EXPECT_EQ(db.stats().record_count, static_cast<std::uint64_t>(n));
  EXPECT_TRUE(db.index().validate().empty());
  for (int i = 0; i < 5000; ++i) {
    Key k = keys[rng() % keys.size()];
    ASSERT_EQ(db.get(k).value().payload, "payload-" + std::to_string(k));
  }
  EXPECT_EQ(db.get(2).status().code(), Code::NotFound);
  auto cons = db.check_consistency();
  EXPECT_TRUE(cons.consistent) << (cons.problems.empty() ? "" : cons.problems[0]);
}

#include <algorithm>
#include <cstring>
#include <random>

#include "indexcraft/heap_file.hpp"
#include "test_util.hpp"

using namespace indexcraft;
using namespace testutil;

namespace {

std::vector<Record> scan_all(const HeapFile& h) {
  std::vector<Record> out;
  Status st = h.scan([&](const Record& r, std::uint64_t) { out.push_back(r); return true; });
  EXPECT_TRUE(st.ok()) << st.to_string();
  return out;
}

}  // namespace

TEST(HeapFile, CreateEmptyFileWritesHeader) {
  TempDir dir;
  HeapFile h;
  ASSERT_TRUE(h.open(dir.file("a.dat"), true).ok());
  EXPECT_EQ(h.live_count(), 0u);
  EXPECT_EQ(h.file_size(), kFileHeaderSize);
  EXPECT_EQ(h.next_rid(), 1u);
  h.close();
  auto bytes = read_file(dir.file("a.dat"));
  ASSERT_EQ(bytes.size(), kFileHeaderSize);
  EXPECT_EQ(std::memcmp(bytes.data(), kFileMagic, 8), 0);
  EXPECT_EQ(get_u32(bytes, 8), kFormatVersion);
}

TEST(HeapFile, OpenMissingFileWithoutCreateIsNotFound) {
  TempDir dir;
  HeapFile h;
  Status st = h.open(dir.file("missing.dat"), false);
  EXPECT_EQ(st.code(), Code::NotFound);
  EXPECT_FALSE(h.is_open());
}

TEST(HeapFile, InsertAndGetRoundTrip) {
  TempDir dir;
  HeapFile h;
  ASSERT_TRUE(h.open(dir.file("a.dat"), true).ok());
  std::string binary("a\0b\xff\x01 end", 9);
  auto a = h.insert(10, "hello");
  auto b = h.insert(-5, "");
  auto c = h.insert(7, binary);
  ASSERT_TRUE(a.ok() && b.ok() && c.ok());
  EXPECT_EQ(a.value().rid, 1u);
  EXPECT_EQ(b.value().rid, 2u);
  auto ra = h.get(a.value().rid);
  ASSERT_TRUE(ra.ok());
  EXPECT_EQ(ra.value().key, 10);
  EXPECT_EQ(ra.value().payload, "hello");
  auto rb = h.get(b.value().rid);
  ASSERT_TRUE(rb.ok());
  EXPECT_EQ(rb.value().key, -5);
  EXPECT_EQ(rb.value().payload, "");
  auto rc = h.read_at(c.value());
  ASSERT_TRUE(rc.ok());
  EXPECT_EQ(rc.value().payload, binary);  // embedded NUL and high bytes survive
  EXPECT_EQ(h.live_count(), 3u);
}

TEST(HeapFile, PayloadSizeLimits) {
  TempDir dir;
  HeapFile h;
  ASSERT_TRUE(h.open(dir.file("a.dat"), true).ok());
  std::string big(kMaxPayloadBytes, 'z');
  auto ok = h.insert(1, big);
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(h.get(ok.value().rid).value().payload.size(), big.size());
  auto too_big = h.insert(2, std::string(kMaxPayloadBytes + 1, 'z'));
  EXPECT_FALSE(too_big.ok());
  EXPECT_EQ(too_big.status().code(), Code::InvalidArgument);
  EXPECT_EQ(h.live_count(), 1u);
}

TEST(HeapFile, ScanVisitsOnlyLiveRecordsInFileOrder) {
  TempDir dir;
  HeapFile h;
  ASSERT_TRUE(h.open(dir.file("a.dat"), true).ok());
  std::vector<RecordLocator> locs;
  for (Key k = 1; k <= 10; ++k) locs.push_back(h.insert(k * 10, "p" + std::to_string(k)).value());
  ASSERT_TRUE(h.erase(locs[2].rid).ok());
  ASSERT_TRUE(h.erase(locs[7].rid).ok());
  auto recs = scan_all(h);
  ASSERT_EQ(recs.size(), 8u);
  std::vector<Key> keys;
  for (auto& r : recs) keys.push_back(r.key);
  EXPECT_EQ(keys, (std::vector<Key>{10, 20, 40, 50, 60, 70, 90, 100}));
  ScanStats stats;
  ASSERT_TRUE(h.scan([](const Record&, std::uint64_t) { return true; }, &stats).ok());
  EXPECT_EQ(stats.slots_seen, 10u);
  EXPECT_EQ(stats.live_records, 8u);
  // early stop
  int visited = 0;
  ASSERT_TRUE(h.scan([&](const Record&, std::uint64_t) { return ++visited < 3; }).ok());
  EXPECT_EQ(visited, 3);
}

TEST(HeapFile, DeleteMakesRecordUnreachable) {
  TempDir dir;
  HeapFile h;
  ASSERT_TRUE(h.open(dir.file("a.dat"), true).ok());
  auto loc = h.insert(1, "x").value();
  ASSERT_TRUE(h.erase(loc.rid).ok());
  EXPECT_EQ(h.get(loc.rid).status().code(), Code::NotFound);
  EXPECT_EQ(h.erase(loc.rid).code(), Code::NotFound);  // second delete
  EXPECT_EQ(h.read_at(loc).status().code(), Code::NotFound);  // via a stale locator: slot is a tombstone
  EXPECT_EQ(h.live_count(), 0u);
  EXPECT_EQ(h.deleted_slots(), 1u);
}

TEST(HeapFile, DeletedSlotIsReusedWithNewRecordIdAndOldLocatorBecomesStale) {
  TempDir dir;
  HeapFile h;
  ASSERT_TRUE(h.open(dir.file("a.dat"), true).ok());
  auto old_loc = h.insert(1, std::string(100, 'a')).value();
  h.insert(2, "keep");
  const std::uint64_t size_before = h.file_size();
  ASSERT_TRUE(h.erase(old_loc.rid).ok());
  auto reused = h.insert(3, std::string(60, 'b'));  // fits (60 <= 100 <= 2*60+64)
  ASSERT_TRUE(reused.ok());
  EXPECT_EQ(reused.value().offset, old_loc.offset);       // same physical slot
  EXPECT_NE(reused.value().rid, old_loc.rid);             // but a new, never-used record id
  EXPECT_EQ(h.file_size(), size_before);                  // file did not grow
  EXPECT_EQ(h.deleted_slots(), 0u);
  EXPECT_EQ(h.get(reused.value().rid).value().payload, std::string(60, 'b'));
  auto stale = h.read_at(old_loc);                        // old locator now points at someone else's record
  EXPECT_FALSE(stale.ok());
  EXPECT_EQ(stale.status().code(), Code::Corrupt);
  EXPECT_NE(stale.status().message().find("stale locator"), std::string::npos);
}

TEST(HeapFile, SlotMuchLargerThanPayloadIsNotReused) {
  TempDir dir;
  HeapFile h;
  ASSERT_TRUE(h.open(dir.file("a.dat"), true).ok());
  auto big = h.insert(1, std::string(1000, 'a')).value();
  ASSERT_TRUE(h.erase(big.rid).ok());
  const std::uint64_t size_before = h.file_size();
  auto small = h.insert(2, "tiny");
  ASSERT_TRUE(small.ok());
  EXPECT_NE(small.value().offset, big.offset);  // appended instead of wasting a 1000-byte slot
  EXPECT_GT(h.file_size(), size_before);
  EXPECT_EQ(h.deleted_slots(), 1u);
}

TEST(HeapFile, ReopenRestoresLiveViewAndNeverReusesRecordIds) {
  TempDir dir;
  const std::string path = dir.file("a.dat");
  RecordId last_rid = 0;
  std::vector<Record> expected;
  {
    HeapFile h;
    ASSERT_TRUE(h.open(path, true).ok());
    std::mt19937_64 rng(3);
    std::vector<RecordLocator> locs;
    for (Key k = 0; k < 100; ++k) locs.push_back(h.insert(k, random_payload(rng)).value());
    for (Key k = 0; k < 100; k += 10) ASSERT_TRUE(h.erase(locs[k].rid).ok());
    ASSERT_TRUE(h.erase(locs[99].rid).ok());  // delete the highest record id too
    last_rid = locs[99].rid;
    expected = scan_all(h);
    EXPECT_EQ(h.live_count(), 89u);
  }
  HeapFile h;
  ASSERT_TRUE(h.open(path, false).ok());
  EXPECT_EQ(h.live_count(), 89u);
  EXPECT_EQ(h.deleted_slots(), 11u);
  EXPECT_GT(h.next_rid(), last_rid);  // tombstone keeps the id burned
  auto after = scan_all(h);
  ASSERT_EQ(after.size(), expected.size());
  for (std::size_t i = 0; i < after.size(); ++i) {
    EXPECT_EQ(after[i].key, expected[i].key);
    EXPECT_EQ(after[i].rid, expected[i].rid);
    EXPECT_EQ(after[i].payload, expected[i].payload);
  }
  // free slots are rediscovered: a same-size insert reuses one
  const std::uint64_t size_before = h.file_size();
  auto r = h.insert(5000, std::string(40, 'q'));
  ASSERT_TRUE(r.ok());
  EXPECT_LE(h.file_size(), size_before + kSlotHeaderSize + 40);
}

TEST(HeapFile, SecondOpenOfSameFileIsRejectedWhileLocked) {
  TempDir dir;
  HeapFile a, b;
  ASSERT_TRUE(a.open(dir.file("a.dat"), true).ok());
  Status st = b.open(dir.file("a.dat"), false);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Code::IoError);
  EXPECT_NE(st.message().find("locked"), std::string::npos);
  a.close();
  EXPECT_TRUE(b.open(dir.file("a.dat"), false).ok());
}

TEST(HeapFile, ZeroLengthFileIsInitialisedOnlyWhenCreating) {
  TempDir dir;
  write_file(dir.file("empty.dat"), {});
  HeapFile h;
  EXPECT_EQ(h.open(dir.file("empty.dat"), false).code(), Code::Corrupt);
  EXPECT_TRUE(h.open(dir.file("empty.dat"), true).ok());
  EXPECT_EQ(h.file_size(), kFileHeaderSize);
}

// ---------------------------------------------------------------- malformed files

class MalformedFile : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = dir_.file("m.dat");
    HeapFile h;
    ASSERT_TRUE(h.open(path_, true).ok());
    for (int i = 0; i < 3; ++i) locs_.push_back(h.insert(100 + i, std::string(10 + 10 * i, static_cast<char>('a' + i))).value());
    h.close();
    good_ = read_file(path_);
    ASSERT_EQ(good_.size(), kFileHeaderSize + (kSlotHeaderSize + 10) + (kSlotHeaderSize + 20) + (kSlotHeaderSize + 30));
  }
  Status open_with(const std::vector<std::uint8_t>& bytes) {
    write_file(path_, bytes);
    HeapFile h;
    return h.open(path_, false);
  }
  std::size_t slot(int i) const { return static_cast<std::size_t>(locs_[i].offset); }

  TempDir dir_;
  std::string path_;
  std::vector<std::uint8_t> good_;
  std::vector<RecordLocator> locs_;
};

TEST_F(MalformedFile, UntouchedFileOpens) { EXPECT_TRUE(open_with(good_).ok()); }

TEST_F(MalformedFile, BadFileMagic) {
  auto b = good_;
  b[0] = 'X';
  Status st = open_with(b);
  EXPECT_EQ(st.code(), Code::Corrupt);
  EXPECT_NE(st.message().find("magic"), std::string::npos);
}

TEST_F(MalformedFile, FileHeaderCrcMismatch) {
  auto b = good_;
  b[30] ^= 0x01;  // inside the CRC-protected header bytes
  EXPECT_EQ(open_with(b).code(), Code::Corrupt);
}

TEST_F(MalformedFile, UnsupportedFormatVersion) {
  auto b = good_;
  put_u32(b, 8, 2);
  fix_header_crc(b);
  EXPECT_EQ(open_with(b).code(), Code::Unsupported);
}

TEST_F(MalformedFile, FileShorterThanHeader) {
  std::vector<std::uint8_t> b(good_.begin(), good_.begin() + 30);
  EXPECT_EQ(open_with(b).code(), Code::Corrupt);
}

TEST_F(MalformedFile, TruncatedInsideLastPayload) {
  std::vector<std::uint8_t> b(good_.begin(), good_.end() - 5);
  Status st = open_with(b);
  EXPECT_EQ(st.code(), Code::Corrupt);
  EXPECT_NE(st.message().find("truncated"), std::string::npos);
  EXPECT_NE(st.message().find("offset " + std::to_string(slot(2))), std::string::npos) << st.message();
}

TEST_F(MalformedFile, TruncatedInsideSlotHeader) {
  auto b = good_;
  for (int i = 0; i < 10; ++i) b.push_back(0xAB);  // partial slot header after the last record
  Status st = open_with(b);
  EXPECT_EQ(st.code(), Code::Corrupt);
  EXPECT_NE(st.message().find("truncated"), std::string::npos);
}

TEST_F(MalformedFile, FlippedPayloadByteFailsCrc) {
  auto b = good_;
  b[slot(1) + kSlotHeaderSize + 3] ^= 0x40;
  Status st = open_with(b);
  EXPECT_EQ(st.code(), Code::Corrupt);
  EXPECT_NE(st.message().find("CRC"), std::string::npos);
  EXPECT_NE(st.message().find("offset " + std::to_string(slot(1))), std::string::npos);
}

TEST_F(MalformedFile, FlippedKeyByteFailsCrc) {
  auto b = good_;
  b[slot(0) + 16] ^= 0x01;
  EXPECT_EQ(open_with(b).code(), Code::Corrupt);
}

TEST_F(MalformedFile, BadSlotMagic) {
  auto b = good_;
  b[slot(2)] = 'X';
  Status st = open_with(b);
  EXPECT_EQ(st.code(), Code::Corrupt);
  EXPECT_NE(st.message().find("magic"), std::string::npos);
}

TEST_F(MalformedFile, InvalidStatusByte) {
  auto b = good_;
  b[slot(1) + 4] = 2;
  EXPECT_EQ(open_with(b).code(), Code::Corrupt);
}

TEST_F(MalformedFile, NonzeroReservedBytes) {
  auto b = good_;
  b[slot(0) + 6] = 1;
  EXPECT_EQ(open_with(b).code(), Code::Corrupt);
}

TEST_F(MalformedFile, LengthLargerThanCapacity) {
  auto b = good_;
  put_u32(b, slot(0) + 28, 11);  // capacity is 10
  fix_slot_crc(b, slot(0));
  Status st = open_with(b);
  EXPECT_EQ(st.code(), Code::Corrupt);
  EXPECT_NE(st.message().find("exceeds capacity"), std::string::npos);
}

TEST_F(MalformedFile, AbsurdCapacityIsRejectedWithoutAllocatingIt) {
  auto b = good_;
  put_u32(b, slot(0) + 24, 0x7FFFFFFF);
  Status st = open_with(b);
  EXPECT_EQ(st.code(), Code::Corrupt);
}

TEST_F(MalformedFile, CapacityPastEndOfFile) {
  auto b = good_;
  put_u32(b, slot(2) + 24, 5000);
  put_u32(b, slot(2) + 28, 5000);
  fix_slot_crc(b, slot(2));
  Status st = open_with(b);
  EXPECT_EQ(st.code(), Code::Corrupt);
  EXPECT_NE(st.message().find("truncated"), std::string::npos);
}

TEST_F(MalformedFile, DuplicateLiveRecordId) {
  auto b = good_;
  put_u64(b, slot(1) + 8, locs_[0].rid);
  fix_slot_crc(b, slot(1));
  Status st = open_with(b);
  EXPECT_EQ(st.code(), Code::Corrupt);
  EXPECT_NE(st.message().find("duplicate live record id"), std::string::npos);
}

TEST_F(MalformedFile, FailedOpenLeavesFileUntouchedAndHandleClosed) {
  auto b = good_;
  b[slot(1) + kSlotHeaderSize] ^= 0x01;
  write_file(path_, b);
  HeapFile h;
  EXPECT_FALSE(h.open(path_, false).ok());
  EXPECT_FALSE(h.is_open());
  EXPECT_EQ(read_file(path_), b);  // never "repaired" silently
}

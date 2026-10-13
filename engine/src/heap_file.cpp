#include "indexcraft/heap_file.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <vector>

#include "indexcraft/crc32.hpp"

namespace indexcraft {

const char kFileMagic[8] = {'I', 'N', 'D', 'X', 'C', 'R', 'F', 'T'};

namespace {

constexpr std::size_t kScanChunk = 1u << 20;       // sequential read-ahead buffer
constexpr std::size_t kSpeculativeRead = kSlotHeaderSize + 256;  // first pread of a point read
constexpr std::uint64_t kSlotReuseSlackFactor = 2;  // reuse slot only if capacity <= 2*len + 64
constexpr std::uint64_t kSlotReuseSlackBytes = 64;

// ---- explicit little-endian helpers (format is independent of host endianness) ----
void put_u32(std::uint8_t* p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put_u64(std::uint8_t* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t get_u32(const std::uint8_t* p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
  return v;
}
std::uint64_t get_u64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return v;
}

std::string at(std::uint64_t offset) { return " at offset " + std::to_string(offset); }

struct ParsedHeader {
  RecordStatus status;
  RecordId rid;
  Key key;
  std::uint32_t capacity;
  std::uint32_t length;
  std::uint32_t crc;
};

// Validates the 36-byte slot header. `remaining` is the number of bytes from the
// slot start to the end of the file.
Status parse_slot_header(const std::uint8_t* h, std::uint64_t offset, std::uint64_t remaining,
                         ParsedHeader* out) {
  if (get_u32(h) != kSlotMagic) {
    return Status::Error(Code::Corrupt, "bad slot magic" + at(offset));
  }
  if (h[4] > 1) {
    return Status::Error(Code::Corrupt, "invalid status byte " + std::to_string(h[4]) + at(offset));
  }
  if (h[5] != 0 || h[6] != 0 || h[7] != 0) {
    return Status::Error(Code::Corrupt, "nonzero reserved bytes in slot header" + at(offset));
  }
  out->status = h[4] == 1 ? RecordStatus::Live : RecordStatus::Deleted;
  out->rid = get_u64(h + 8);
  out->key = static_cast<Key>(get_u64(h + 16));
  out->capacity = get_u32(h + 24);
  out->length = get_u32(h + 28);
  out->crc = get_u32(h + 32);
  if (out->capacity > kMaxPayloadBytes) {
    return Status::Error(Code::Corrupt, "slot capacity " + std::to_string(out->capacity) +
                                            " exceeds maximum" + at(offset));
  }
  if (out->length > out->capacity) {
    return Status::Error(Code::Corrupt, "slot length " + std::to_string(out->length) +
                                            " exceeds capacity " + std::to_string(out->capacity) +
                                            at(offset));
  }
  if (remaining < static_cast<std::uint64_t>(kSlotHeaderSize) + out->capacity) {
    return Status::Error(Code::Corrupt, "truncated record: slot needs " +
                                            std::to_string(kSlotHeaderSize + out->capacity) +
                                            " bytes but only " + std::to_string(remaining) +
                                            " remain" + at(offset));
  }
  return Status::Ok();
}

std::uint32_t slot_crc(const std::uint8_t* header, const std::uint8_t* payload, std::uint32_t length) {
  std::uint32_t c = crc32(header + 8, 24);
  return crc32(payload, length, c);
}

}  // namespace

HeapFile::~HeapFile() { close(); }

void HeapFile::reset_state() {
  file_size_ = 0;
  live_count_ = 0;
  deleted_slots_ = 0;
  next_rid_ = 1;
  rid_to_offset_.clear();
  free_slots_.clear();
  read_calls_ = 0;
  read_bytes_ = 0;
}

void HeapFile::close() {
  if (fd_ >= 0) {
    ::close(fd_);  // also releases the flock
    fd_ = -1;
  }
  reset_state();
  path_.clear();
}

Status HeapFile::sync() {
  if (!is_open()) return Status::Error(Code::InvalidArgument, "heap file is not open");
  if (::fsync(fd_) != 0) return Status::Error(Code::IoError, std::string("fsync failed: ") + std::strerror(errno));
  return Status::Ok();
}

Status HeapFile::read_exact(void* buf, std::size_t n, std::uint64_t offset) const {
  auto* p = static_cast<std::uint8_t*>(buf);
  std::size_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd_, p + done, n - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return Status::Error(Code::IoError, std::string("pread failed: ") + std::strerror(errno) + at(offset));
    }
    if (r == 0) return Status::Error(Code::IoError, "unexpected end of file" + at(offset + done));
    done += static_cast<std::size_t>(r);
  }
  return Status::Ok();
}

Status HeapFile::write_exact(const void* buf, std::size_t n, std::uint64_t offset) {
  const auto* p = static_cast<const std::uint8_t*>(buf);
  std::size_t done = 0;
  while (done < n) {
    ssize_t r = ::pwrite(fd_, p + done, n - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return Status::Error(Code::IoError, std::string("pwrite failed: ") + std::strerror(errno) + at(offset));
    }
    done += static_cast<std::size_t>(r);
  }
  return Status::Ok();
}

Status HeapFile::open(const std::string& path, bool create_if_missing, const LiveVisitor& on_live) {
  if (is_open()) return Status::Error(Code::InvalidArgument, "heap file is already open");
  if (path.empty()) return Status::Error(Code::InvalidArgument, "empty path");

  int flags = O_RDWR | O_CLOEXEC | (create_if_missing ? O_CREAT : 0);
  int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    if (errno == ENOENT) return Status::Error(Code::NotFound, "data file does not exist: " + path);
    return Status::Error(Code::IoError, "cannot open " + path + ": " + std::strerror(errno));
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int e = errno;
    ::close(fd);
    if (e == EWOULDBLOCK) {
      return Status::Error(Code::IoError, "data file is locked by another process: " + path);
    }
    return Status::Error(Code::IoError, std::string("flock failed: ") + std::strerror(e));
  }
  fd_ = fd;
  path_ = path;
  reset_state();

  auto fail = [&](Status st) {
    ::close(fd_);
    fd_ = -1;
    reset_state();
    path_.clear();
    return st;
  };

  struct stat sb;
  if (::fstat(fd_, &sb) != 0) return fail(Status::Error(Code::IoError, std::string("fstat failed: ") + std::strerror(errno)));
  file_size_ = static_cast<std::uint64_t>(sb.st_size);

  if (file_size_ == 0) {
    if (!create_if_missing) return fail(Status::Error(Code::Corrupt, "data file is empty (no file header)"));
    std::uint8_t hdr[kFileHeaderSize] = {0};
    std::memcpy(hdr, kFileMagic, 8);
    put_u32(hdr + 8, kFormatVersion);
    put_u32(hdr + 12, kFileHeaderSize);
    put_u32(hdr + 16, kSlotHeaderSize);
    put_u32(hdr + 20, 0);
    put_u64(hdr + 24, static_cast<std::uint64_t>(std::time(nullptr)));
    put_u32(hdr + 60, crc32(hdr, 60));
    Status st = write_exact(hdr, sizeof hdr, 0);
    if (!st.ok()) return fail(st);
    file_size_ = kFileHeaderSize;
  } else {
    if (file_size_ < kFileHeaderSize) {
      return fail(Status::Error(Code::Corrupt, "file is shorter than the 64-byte header (" +
                                                   std::to_string(file_size_) + " bytes)"));
    }
    std::uint8_t hdr[kFileHeaderSize];
    Status st = read_exact(hdr, sizeof hdr, 0);
    if (!st.ok()) return fail(st);
    if (std::memcmp(hdr, kFileMagic, 8) != 0) return fail(Status::Error(Code::Corrupt, "bad file magic: not an IndexCraft data file"));
    if (get_u32(hdr + 60) != crc32(hdr, 60)) return fail(Status::Error(Code::Corrupt, "file header CRC mismatch"));
    if (get_u32(hdr + 8) != kFormatVersion) {
      return fail(Status::Error(Code::Unsupported, "unsupported format version " + std::to_string(get_u32(hdr + 8))));
    }
    if (get_u32(hdr + 12) != kFileHeaderSize || get_u32(hdr + 16) != kSlotHeaderSize) {
      return fail(Status::Error(Code::Corrupt, "file header declares unexpected header sizes"));
    }
  }

  // Single validating pass: build rid map, free list, counters, and (optionally) the index.
  Status st = walk(
      [&](const SlotView& s, Status& err) {
        if (s.rid >= next_rid_) next_rid_ = s.rid + 1;
        if (s.status == RecordStatus::Live) {
          if (!rid_to_offset_.emplace(s.rid, s.offset).second) {
            err = Status::Error(Code::Corrupt, "duplicate live record id " + std::to_string(s.rid) + at(s.offset));
            return false;
          }
          ++live_count_;
          if (on_live) {
            Status v = on_live(s.key, RecordLocator{s.rid, s.offset});
            if (!v.ok()) {
              err = v;
              return false;
            }
          }
        } else {
          free_slots_.emplace(s.capacity, s.offset);
          ++deleted_slots_;
        }
        return true;
      },
      nullptr);
  if (!st.ok()) return fail(st);
  return Status::Ok();
}

Status HeapFile::walk(const SlotCallback& cb, std::uint64_t* bytes_read) const {
  std::vector<std::uint8_t> buf;
  std::uint64_t buf_off = 0;
  std::size_t buf_len = 0;

  // Make [off, off+n) available in buf. Precondition: off+n <= file_size_.
  auto ensure = [&](std::uint64_t off, std::size_t n) -> Status {
    if (off >= buf_off && off + n <= buf_off + buf_len) return Status::Ok();
    std::uint64_t remain = file_size_ - off;
    std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(std::max(kScanChunk, n), remain));
    if (buf.size() < want) buf.resize(want);
    Status st = read_exact(buf.data(), want, off);
    if (!st.ok()) return st;
    if (bytes_read) *bytes_read += want;
    buf_off = off;
    buf_len = want;
    return Status::Ok();
  };

  std::uint64_t off = kFileHeaderSize;
  while (off < file_size_) {
    std::uint64_t remain = file_size_ - off;
    if (remain < kSlotHeaderSize) {
      return Status::Error(Code::Corrupt, "truncated slot header: " + std::to_string(remain) +
                                              " trailing bytes" + at(off));
    }
    Status st = ensure(off, kSlotHeaderSize);
    if (!st.ok()) return st;
    ParsedHeader h;
    st = parse_slot_header(buf.data() + (off - buf_off), off, remain, &h);
    if (!st.ok()) return st;
    std::uint64_t total = static_cast<std::uint64_t>(kSlotHeaderSize) + h.capacity;
    st = ensure(off, static_cast<std::size_t>(total));
    if (!st.ok()) return st;
    const std::uint8_t* slot = buf.data() + (off - buf_off);
    const std::uint8_t* payload = slot + kSlotHeaderSize;
    if (slot_crc(slot, payload, h.length) != h.crc) {
      return Status::Error(Code::Corrupt, "CRC mismatch (record data is damaged)" + at(off));
    }
    SlotView view{off, h.status, h.rid, h.key, h.capacity, h.length, payload};
    Status err;
    if (!cb(view, err)) return err;
    off += total;
  }
  return Status::Ok();
}

Result<RecordLocator> HeapFile::insert(Key key, const std::string& payload) {
  if (!is_open()) return Status::Error(Code::InvalidArgument, "heap file is not open");
  if (payload.size() > kMaxPayloadBytes) {
    return Status::Error(Code::InvalidArgument, "payload of " + std::to_string(payload.size()) +
                                                    " bytes exceeds the maximum of " +
                                                    std::to_string(kMaxPayloadBytes));
  }
  const std::uint32_t len = static_cast<std::uint32_t>(payload.size());

  auto reuse_it = free_slots_.lower_bound(len);  // smallest free slot that fits (best fit)
  bool reuse = reuse_it != free_slots_.end() &&
               reuse_it->first <= kSlotReuseSlackFactor * len + kSlotReuseSlackBytes;
  std::uint32_t capacity = reuse ? reuse_it->first : len;
  std::uint64_t offset = reuse ? reuse_it->second : file_size_;
  const RecordId rid = next_rid_;

  std::vector<std::uint8_t> slot(static_cast<std::size_t>(kSlotHeaderSize) + capacity, 0);
  put_u32(slot.data(), kSlotMagic);
  slot[4] = 1;
  put_u64(slot.data() + 8, rid);
  put_u64(slot.data() + 16, static_cast<std::uint64_t>(key));
  put_u32(slot.data() + 24, capacity);
  put_u32(slot.data() + 28, len);
  if (len) std::memcpy(slot.data() + kSlotHeaderSize, payload.data(), len);
  put_u32(slot.data() + 32, slot_crc(slot.data(), slot.data() + kSlotHeaderSize, len));

  Status st = write_exact(slot.data(), slot.size(), offset);
  if (!st.ok()) {
    if (!reuse) {
      if (::ftruncate(fd_, static_cast<off_t>(file_size_)) != 0) { /* best effort */ }
    }
    return st;
  }
  if (reuse) {
    free_slots_.erase(reuse_it);
    --deleted_slots_;
  } else {
    file_size_ += slot.size();
  }
  ++next_rid_;
  rid_to_offset_[rid] = offset;
  ++live_count_;
  return RecordLocator{rid, offset};
}

Result<Record> HeapFile::read_at(const RecordLocator& loc) const {
  if (!is_open()) return Status::Error(Code::InvalidArgument, "heap file is not open");
  if (loc.offset < kFileHeaderSize || loc.offset + kSlotHeaderSize > file_size_) {
    return Status::Error(Code::Corrupt, "record locator outside file bounds" + at(loc.offset));
  }
  std::uint64_t remaining = file_size_ - loc.offset;
  std::size_t first = static_cast<std::size_t>(std::min<std::uint64_t>(kSpeculativeRead, remaining));
  std::vector<std::uint8_t> buf(first);
  Status st = read_exact(buf.data(), first, loc.offset);
  ++read_calls_;
  read_bytes_ += first;
  if (!st.ok()) return st;

  ParsedHeader h;
  st = parse_slot_header(buf.data(), loc.offset, remaining, &h);
  if (!st.ok()) return st;
  std::size_t total = static_cast<std::size_t>(kSlotHeaderSize) + h.capacity;
  if (total > first) {
    buf.resize(total);
    st = read_exact(buf.data() + first, total - first, loc.offset + first);
    ++read_calls_;
    read_bytes_ += total - first;
    if (!st.ok()) return st;
  }
  if (slot_crc(buf.data(), buf.data() + kSlotHeaderSize, h.length) != h.crc) {
    return Status::Error(Code::Corrupt, "CRC mismatch (record data is damaged)" + at(loc.offset));
  }
  if (h.rid != loc.rid) {
    return Status::Error(Code::Corrupt, "stale locator: slot holds record " + std::to_string(h.rid) +
                                            ", expected " + std::to_string(loc.rid) + at(loc.offset));
  }
  if (h.status != RecordStatus::Live) {
    return Status::Error(Code::NotFound, "record " + std::to_string(loc.rid) + " is deleted");
  }
  Record r;
  r.key = h.key;
  r.rid = h.rid;
  r.payload.assign(reinterpret_cast<const char*>(buf.data() + kSlotHeaderSize), h.length);
  return r;
}

Result<Record> HeapFile::get(RecordId rid) const {
  auto it = rid_to_offset_.find(rid);
  if (it == rid_to_offset_.end()) {
    return Status::Error(Code::NotFound, "no live record with id " + std::to_string(rid));
  }
  return read_at(RecordLocator{rid, it->second});
}

Status HeapFile::erase(RecordId rid) {
  if (!is_open()) return Status::Error(Code::InvalidArgument, "heap file is not open");
  auto it = rid_to_offset_.find(rid);
  if (it == rid_to_offset_.end()) {
    return Status::Error(Code::NotFound, "no live record with id " + std::to_string(rid));
  }
  const std::uint64_t offset = it->second;
  std::uint8_t hdr[kSlotHeaderSize];
  Status st = read_exact(hdr, sizeof hdr, offset);
  if (!st.ok()) return st;
  if (get_u32(hdr) != kSlotMagic || hdr[4] != 1 || get_u64(hdr + 8) != rid) {
    return Status::Error(Code::Corrupt, "slot does not match record " + std::to_string(rid) + at(offset));
  }
  const std::uint8_t zero = 0;
  st = write_exact(&zero, 1, offset + 4);  // flip status byte in place
  if (!st.ok()) return st;
  free_slots_.emplace(get_u32(hdr + 24), offset);
  rid_to_offset_.erase(it);
  --live_count_;
  ++deleted_slots_;
  return Status::Ok();
}

Status HeapFile::scan_raw(const RawVisitor& visitor, ScanStats* stats) const {
  if (!is_open()) return Status::Error(Code::InvalidArgument, "heap file is not open");
  ScanStats local;
  std::uint64_t bytes = 0;
  Status st = walk(
      [&](const SlotView& s, Status&) {
        ++local.slots_seen;
        if (s.status != RecordStatus::Live) return true;
        ++local.live_records;
        return visitor(s.key, s.rid, reinterpret_cast<const char*>(s.payload), s.length, s.offset);
      },
      &bytes);
  local.bytes_read = bytes;
  if (stats) *stats = local;
  return st;
}

Status HeapFile::scan(const RecordVisitor& visitor, ScanStats* stats) const {
  return scan_raw(
      [&](Key key, RecordId rid, const char* payload, std::uint32_t len, std::uint64_t offset) {
        Record r;
        r.key = key;
        r.rid = rid;
        r.payload.assign(payload, len);
        return visitor(r, offset);
      },
      stats);
}

}  // namespace indexcraft

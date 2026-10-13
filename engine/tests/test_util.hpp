#pragma once
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "indexcraft/crc32.hpp"
#include "indexcraft/heap_file.hpp"

namespace testutil {

// A unique temporary directory removed when the object is destroyed.
class TempDir {
 public:
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "indexcraft_test_XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* r = mkdtemp(buf.data());
    if (!r) throw std::runtime_error("mkdtemp failed");
    path_ = r;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  std::string file(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

inline std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline std::uint32_t get_u32(const std::vector<std::uint8_t>& b, std::size_t off) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[off + i]) << (8 * i);
  return v;
}
inline void put_u32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void put_u64(std::vector<std::uint8_t>& b, std::size_t off, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// Recomputes the CRC of the slot starting at `slot_off` (after a test edited its fields).
inline void fix_slot_crc(std::vector<std::uint8_t>& b, std::size_t slot_off) {
  std::uint32_t length = get_u32(b, slot_off + 28);
  std::uint32_t c = indexcraft::crc32(b.data() + slot_off + 8, 24);
  c = indexcraft::crc32(b.data() + slot_off + indexcraft::kSlotHeaderSize, length, c);
  put_u32(b, slot_off + 32, c);
}

inline void fix_header_crc(std::vector<std::uint8_t>& b) { put_u32(b, 60, indexcraft::crc32(b.data(), 60)); }

inline std::string random_payload(std::mt19937_64& rng, std::size_t max_len = 120) {
  std::size_t len = rng() % (max_len + 1);
  std::string s(len, 'x');
  for (char& c : s) c = static_cast<char>('a' + rng() % 26);
  return s;
}

}  // namespace testutil

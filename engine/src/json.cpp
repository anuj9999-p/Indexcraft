#include "indexcraft/json.hpp"

#include <cstdio>

namespace indexcraft {

namespace {
// Returns the length (1-4) of a valid UTF-8 sequence starting at s[i], or 0 if invalid.
std::size_t utf8_len(const std::string& s, std::size_t i) {
  const auto b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) return 1;
  std::size_t need = 0;
  unsigned cp = 0;
  if ((b0 & 0xE0) == 0xC0) { need = 1; cp = b0 & 0x1F; }
  else if ((b0 & 0xF0) == 0xE0) { need = 2; cp = b0 & 0x0F; }
  else if ((b0 & 0xF8) == 0xF0) { need = 3; cp = b0 & 0x07; }
  else return 0;
  if (i + need >= s.size()) return 0;  // continuation bytes must exist: indices i+1..i+need
  for (std::size_t k = 1; k <= need; ++k) {
    const auto b = static_cast<unsigned char>(s[i + k]);
    if ((b & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (b & 0x3F);
  }
  if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000)) return 0;  // overlong
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return need + 1;
}
}  // namespace

std::string json_quote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == '"') { out += "\\\""; ++i; }
    else if (c == '\\') { out += "\\\\"; ++i; }
    else if (c == '\n') { out += "\\n"; ++i; }
    else if (c == '\r') { out += "\\r"; ++i; }
    else if (c == '\t') { out += "\\t"; ++i; }
    else if (c < 0x20 || c == 0x7F) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
      ++i;
    } else if (c < 0x80) { out.push_back(static_cast<char>(c)); ++i; }
    else {
      std::size_t n = utf8_len(s, i);
      if (n == 0) { out += "\\ufffd"; ++i; }
      else { out.append(s, i, n); i += n; }
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace indexcraft

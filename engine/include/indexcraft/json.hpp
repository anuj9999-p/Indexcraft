#pragma once
#include <string>

namespace indexcraft {

// Returns `s` as a quoted JSON string. Control characters are escaped. Bytes that are
// not valid UTF-8 are replaced with U+FFFD so the output is always valid JSON text
// (the stored payload bytes themselves are never modified).
std::string json_quote(const std::string& s);

}  // namespace indexcraft

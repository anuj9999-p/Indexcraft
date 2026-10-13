#include "indexcraft/build_info.hpp"

#include <sstream>

#include "indexcraft/build_config.hpp"
#include "indexcraft/heap_file.hpp"
#include "indexcraft/json.hpp"
#include "indexcraft/query.hpp"

namespace indexcraft {

std::string build_info_json() {
  std::ostringstream os;
  os << "{\"version\":" << json_quote(INDEXCRAFT_VERSION)
     << ",\"build_type\":" << json_quote(INDEXCRAFT_BUILD_TYPE)
     << ",\"compiler\":" << json_quote(INDEXCRAFT_COMPILER)
     << ",\"cxx_flags\":" << json_quote(INDEXCRAFT_CXX_FLAGS)
     << ",\"cxx_standard\":" << __cplusplus
     << ",\"storage_format_version\":" << kFormatVersion
     << ",\"max_payload_bytes\":" << kMaxPayloadBytes
     << ",\"scan_selectivity_threshold\":" << kScanSelectivityThreshold << "}";
  return os.str();
}

}  // namespace indexcraft

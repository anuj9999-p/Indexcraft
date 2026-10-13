#pragma once
#include <string>

namespace indexcraft {

// JSON object describing how this binary was built (version, compiler, flags, format
// version, planner threshold). Shown on the Diagnostics page.
std::string build_info_json();

}  // namespace indexcraft

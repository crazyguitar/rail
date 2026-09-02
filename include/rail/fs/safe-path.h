#pragma once

#include "rail/result.h"

#include <filesystem>
#include <string>

namespace rail {

Result<std::filesystem::path> underRoot(const std::filesystem::path &Root, const std::string &Name);

// What a mount may name as its export, checked on the near side against the
// rule underRoot enforces on the far one.
Result<std::string> exportRoot(const std::string &Given);

} // namespace rail

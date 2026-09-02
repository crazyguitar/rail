#include "rail/fs/safe-path.h"

#include <format>

namespace rail {

Result<std::filesystem::path> underRoot(const std::filesystem::path &Root, const std::string &Name) {
  if (Name.empty()) return failMessage("refusing empty path from peer");

  const std::filesystem::path Relative(Name);
  if (Relative.is_absolute()) return failMessage(std::format("refusing absolute path from peer: {}", Name));
  for (const auto &Part : Relative)
    if (Part == "..") return failMessage(std::format("refusing parent reference from peer: {}", Name));

  const std::filesystem::path Candidate = Root / Relative;

  std::error_code EC;
  auto RealRoot = std::filesystem::weakly_canonical(Root, EC);
  if (EC) RealRoot = Root;
  auto Real = std::filesystem::weakly_canonical(Candidate, EC);
  if (EC) Real = Candidate;

  const std::string RootText = RealRoot.generic_string();
  const std::string RealText = Real.generic_string();
  if (RealText != RootText && !RealText.starts_with(RootText + "/")) return failMessage(std::format("refusing path outside the root: {}", Name));

  return Candidate;
}

Result<std::string> exportRoot(const std::string &Given) {
  std::string Trimmed = Given;
  while (!Trimmed.empty() && Trimmed.back() == '/') Trimmed.pop_back();

  if (Trimmed.empty()) return std::string{"."};
  if (Trimmed.front() == '/')
    return fail(std::make_error_code(std::errc::invalid_argument), "export is relative to what the daemon serves; use / for the whole of it");

  return Trimmed;
}

} // namespace rail

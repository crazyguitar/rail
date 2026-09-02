#pragma once

#include "rail/result.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rail {

class FilterRules {
public:
  void addExclude(const std::string &Pattern) { add(Pattern, false); }
  void addInclude(const std::string &Pattern) { add(Pattern, true); }

  Result<void> addRule(const std::string &Line);
  Result<void> addFrom(const std::filesystem::path &File, bool Include);

  bool empty() const { return Rules.empty(); }
  bool allows(std::string_view Relative, bool Directory) const;

private:
  struct Rule {
    std::string Pattern;
    bool Include = false;
    bool DirectoryOnly = false;
    bool Anchored = false;
  };

  void add(const std::string &Pattern, bool Include);

  std::vector<Rule> Rules;
};

} // namespace rail

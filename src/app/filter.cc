#include "rail/app/filter.h"

#include <format>
#include <fstream>

namespace rail {

namespace {

bool matchClass(std::string_view Pattern, size_t &P, char C) {
  size_t I = P + 1;
  bool Negated = false;
  if (I < Pattern.size() && (Pattern[I] == '!' || Pattern[I] == '^')) {
    Negated = true;
    I++;
  }

  bool Matched = false;
  bool First = true;
  for (; I < Pattern.size(); I++) {
    if (Pattern[I] == ']' && !First) break;
    First = false;

    if (I + 2 < Pattern.size() && Pattern[I + 1] == '-' && Pattern[I + 2] != ']') {
      if (C >= Pattern[I] && C <= Pattern[I + 2]) Matched = true;
      I += 2;
      continue;
    }
    if (Pattern[I] == C) Matched = true;
  }

  if (I >= Pattern.size()) return false;
  P = I;
  return Matched != Negated;
}

bool globMatch(std::string_view Pattern, std::string_view Text) {
  size_t P = 0;
  size_t T = 0;

  while (P < Pattern.size()) {
    const char C = Pattern[P];

    if (C == '*') {
      const bool CrossesSlash = P + 1 < Pattern.size() && Pattern[P + 1] == '*';
      size_t Next = P + (CrossesSlash ? 2 : 1);
      while (CrossesSlash && Next < Pattern.size() && Pattern[Next] == '*') Next++;

      for (size_t Skip = T;; Skip++) {
        if (globMatch(Pattern.substr(Next), Text.substr(Skip))) return true;
        if (Skip >= Text.size()) return false;
        if (!CrossesSlash && Text[Skip] == '/') return false;
      }
    }

    if (T >= Text.size()) return false;

    if (C == '?') {
      if (Text[T] == '/') return false;
    } else if (C == '[') {
      if (!matchClass(Pattern, P, Text[T])) return false;
    } else if (C != Text[T]) {
      return false;
    }

    P++;
    T++;
  }
  return T == Text.size();
}

bool matchesAnywhere(const std::string &Pattern, std::string_view Path) {
  for (size_t Start = 0;;) {
    if (globMatch(Pattern, Path.substr(Start))) return true;
    const size_t Slash = Path.find('/', Start);
    if (Slash == std::string_view::npos) return false;
    Start = Slash + 1;
  }
}

std::string trimmed(const std::string &Line) {
  const size_t First = Line.find_first_not_of(" \t\r\n");
  if (First == std::string::npos) return {};
  const size_t Last = Line.find_last_not_of(" \t\r\n");
  return Line.substr(First, Last - First + 1);
}

} // namespace

void FilterRules::add(const std::string &Pattern, bool Include) {
  std::string P = Pattern;
  if (P.empty()) return;

  if (P.size() > 4 && P.ends_with("/***")) {
    add(P.substr(0, P.size() - 4), Include);
    P = P.substr(0, P.size() - 3) + "**";
  }

  Rule R;
  R.Include = Include;

  if (P.ends_with('/')) {
    R.DirectoryOnly = true;
    P.pop_back();
  }
  if (P.starts_with('/')) {
    R.Anchored = true;
    P.erase(0, 1);
  }
  if (P.empty()) return;

  R.Pattern = std::move(P);
  Rules.push_back(std::move(R));
}

Result<void> FilterRules::addRule(const std::string &Line) {
  const std::string Text = trimmed(Line);
  if (Text.empty()) return {};

  if (Text.starts_with("+ ") || Text.starts_with("- ")) {
    add(trimmed(Text.substr(2)), Text[0] == '+');
    return {};
  }
  if (Text.starts_with("include ")) {
    add(trimmed(Text.substr(8)), true);
    return {};
  }
  if (Text.starts_with("exclude ")) {
    add(trimmed(Text.substr(8)), false);
    return {};
  }
  return failMessage(std::format("filter rule must open with '+ ' or '- ': {}", Line));
}

Result<void> FilterRules::addFrom(const std::filesystem::path &File, bool Include) {
  std::ifstream In(File);
  if (!In) return failMessage(std::format("cannot read {}", File.string()));

  std::string Line;
  while (std::getline(In, Line)) {
    const std::string Text = trimmed(Line);
    if (Text.empty() || Text[0] == '#' || Text[0] == ';') continue;
    add(Text, Include);
  }
  return {};
}

bool FilterRules::allows(std::string_view Relative, bool Directory) const {
  for (const Rule &R : Rules) {
    if (R.DirectoryOnly && !Directory) continue;
    if (R.Anchored ? globMatch(R.Pattern, Relative) : matchesAnywhere(R.Pattern, Relative)) return R.Include;
  }
  return true;
}

} // namespace rail

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace yrs {

inline std::string_view trim_start(std::string_view sv) {
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
                         sv.front() == '\n' || sv.front() == '\r')) {
    sv.remove_prefix(1);
  }
  return sv;
}

inline std::string_view trim_end(std::string_view sv) {
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' ||
                         sv.back() == '\n' || sv.back() == '\r')) {
    sv.remove_suffix(1);
  }
  return sv;
}

inline std::string_view trim(std::string_view sv) {
  return trim_end(trim_start(sv));
}

inline bool all_ascii_digits(std::string_view sv) {
  if (sv.empty()) return false;
  for (unsigned char c : sv) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

std::string ascii_lower(std::string_view sv);
std::string ascii_upper(std::string_view sv);
bool ascii_icase_starts_with(std::string_view sv, std::string_view prefix);
std::optional<int64_t> parse_positive_i64(std::string_view sv);
std::optional<int64_t> parse_leading_positive_i64(std::string_view sv);
bool natural_less(std::string_view a, std::string_view b);

}  // namespace yrs

#include "core/str_util.hpp"

#include <cctype>
#include <charconv>

namespace yrs {

std::string ascii_lower(std::string_view sv) {
  std::string out(sv);
  for (char &c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string ascii_upper(std::string_view sv) {
  std::string out(sv);
  for (char &c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

bool ascii_icase_starts_with(std::string_view sv, std::string_view prefix) {
  if (sv.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(sv[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

std::optional<int64_t> parse_positive_i64(std::string_view sv) {
  if (!all_ascii_digits(sv)) return std::nullopt;
  int64_t val = 0;
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
  if (ec != std::errc{} || ptr != sv.data() + sv.size() || val <= 0) {
    return std::nullopt;
  }
  return val;
}

std::optional<int64_t> parse_leading_positive_i64(std::string_view sv) {
  size_t len = 0;
  while (len < sv.size() &&
         std::isdigit(static_cast<unsigned char>(sv[len]))) {
    ++len;
  }
  if (len == 0) return std::nullopt;
  return parse_positive_i64(sv.substr(0, len));
}

bool natural_less(std::string_view a, std::string_view b) {
  size_t ia = 0, ib = 0;
  while (ia < a.size() && ib < b.size()) {
    bool da = std::isdigit(static_cast<unsigned char>(a[ia])) != 0;
    bool db = std::isdigit(static_cast<unsigned char>(b[ib])) != 0;
    if (da && db) {
      size_t sa = ia;
      while (ia < a.size() &&
             std::isdigit(static_cast<unsigned char>(a[ia]))) {
        ++ia;
      }
      size_t sb = ib;
      while (ib < b.size() &&
             std::isdigit(static_cast<unsigned char>(b[ib]))) {
        ++ib;
      }
      size_t nz_a = sa;
      while (nz_a < ia && a[nz_a] == '0') ++nz_a;
      size_t nz_b = sb;
      while (nz_b < ib && b[nz_b] == '0') ++nz_b;

      size_t len_a = ia - nz_a;
      size_t len_b = ib - nz_b;
      if (len_a != len_b) return len_a < len_b;
      int cmp = a.substr(nz_a, len_a).compare(b.substr(nz_b, len_b));
      if (cmp != 0) return cmp < 0;
      size_t raw_len_a = ia - sa;
      size_t raw_len_b = ib - sb;
      if (raw_len_a != raw_len_b) return raw_len_a < raw_len_b;
      continue;
    }
    unsigned char ca = static_cast<unsigned char>(a[ia]);
    unsigned char cb = static_cast<unsigned char>(b[ib]);
    unsigned char la = static_cast<unsigned char>(std::tolower(ca));
    unsigned char lb = static_cast<unsigned char>(std::tolower(cb));
    if (la != lb) return la < lb;
    if (ca != cb) return ca < cb;
    ++ia;
    ++ib;
  }
  return (a.size() - ia) < (b.size() - ib);
}

}  // namespace yrs

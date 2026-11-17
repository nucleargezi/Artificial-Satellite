#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace yrs::term {

inline constexpr std::string_view RED = "\x1b[31m";
inline constexpr std::string_view GREEN = "\x1b[32m";
inline constexpr std::string_view YELLOW = "\x1b[33m";
inline constexpr std::string_view BLUE = "\x1b[94m";
inline constexpr std::string_view DIM = "\x1b[90m";
inline constexpr std::string_view RESET = "\x1b[0m";

inline void append_kv_row(std::string &out, std::string_view label,
                          std::string_view val, size_t width = 15) {
  size_t len = label.size();
  size_t pad = (len < width) ? (width - len) : 0;
  size_t left = pad / 2;
  size_t right = pad - left;
  out.append(left, ' ');
  out.append(label);
  out.append(right, ' ');
  out.push_back(' ');
  out.append(val);
  out.push_back('\n');
}

inline void print_kv_row(std::string_view label, std::string_view val,
                         size_t width = 15) {
  std::string row;
  row.reserve(width + val.size() + 2);
  append_kv_row(row, label, val, width);
  std::fwrite(row.data(), 1, row.size(), stdout);
}

}  // namespace yrs::term

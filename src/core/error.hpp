#pragma once

#include <cerrno>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace yrs {

enum class ErrKind {
  NotFound,
  InvalidData,
  InvalidInput,
  Other,
};

struct Error {
  ErrKind kind;
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

inline Error make_err(ErrKind kind, std::string msg) {
  return Error{kind, std::move(msg)};
}

inline Error from_errno(std::string_view prefix = "") {
  ErrKind k = ErrKind::Other;
  if (errno == ENOENT) k = ErrKind::NotFound;
  else if (errno == EINVAL) k = ErrKind::InvalidInput;
  std::string msg = std::strerror(errno);
  if (!prefix.empty()) {
    std::string full(prefix);
    full.append(": ").append(msg);
    msg = std::move(full);
  }
  return Error{k, std::move(msg)};
}

inline constexpr std::string_view ciallo() {
  return "Ciallo～(∠・ω<)⌒★ from core";
}

}  // namespace yrs

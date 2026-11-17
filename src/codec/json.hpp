#pragma once

#include "core/error.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace yrs::json {

struct Object;

struct Value {
  using Array = std::vector<Value>;
  std::variant<std::monostate, bool, double, std::string, Array,
               std::unique_ptr<Object>>
      data;

  Value() = default;
  ~Value();
  Value(Value &&) noexcept;
  Value &operator=(Value &&) noexcept;
  Value(const Value &) = delete;
  Value &operator=(const Value &) = delete;

  bool is_null() const { return std::holds_alternative<std::monostate>(data); }

  std::optional<bool> as_bool() const {
    if (const auto *p = std::get_if<bool>(&data)) return *p;
    return std::nullopt;
  }

  std::optional<double> as_num() const {
    if (const auto *p = std::get_if<double>(&data)) return *p;
    return std::nullopt;
  }

  std::optional<std::string_view> as_str() const {
    if (const auto *p = std::get_if<std::string>(&data)) {
      return std::string_view(*p);
    }
    return std::nullopt;
  }

  const Array *as_arr() const { return std::get_if<Array>(&data); }

  const Object *as_obj() const {
    if (const auto *p = std::get_if<std::unique_ptr<Object>>(&data)) {
      return p->get();
    }
    return nullptr;
  }

  Object *as_obj_mut() {
    if (auto *p = std::get_if<std::unique_ptr<Object>>(&data)) {
      return p->get();
    }
    return nullptr;
  }

  const Value *get(std::string_view key) const;
  Value *get_mut(std::string_view key);
};

struct Object {
  std::map<std::string, Value> entries;
};

inline Value::~Value() = default;
inline Value::Value(Value &&) noexcept = default;
inline Value &Value::operator=(Value &&) noexcept = default;

Result<Value> parse(std::string_view input);
std::string serialize(const Value &val);

}  // namespace yrs::json

#include "codec/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>

namespace yrs::json {

const Value *Value::get(std::string_view key) const {
  const Object *obj = as_obj();
  if (!obj) return nullptr;
  auto it = obj->entries.find(std::string(key));
  if (it == obj->entries.end()) return nullptr;
  return &it->second;
}

Value *Value::get_mut(std::string_view key) {
  Object *obj = as_obj_mut();
  if (!obj) return nullptr;
  auto it = obj->entries.find(std::string(key));
  if (it == obj->entries.end()) return nullptr;
  return &it->second;
}

namespace {

struct Parser {
  std::string_view sv;
  size_t pos = 0;

  void skip_ws() {
    while (pos < sv.size()) {
      char c = sv[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
      else break;
    }
  }

  static void append_utf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7Fu) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
      out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
      out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
  }

  bool parse_hex4(uint32_t &out) {
    if (pos + 4 > sv.size()) return false;
    uint32_t val = 0;
    for (int i = 0; i < 4; ++i) {
      char c = sv[pos++];
      val <<= 4;
      if (c >= '0' && c <= '9') val |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') val |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') val |= static_cast<uint32_t>(c - 'A' + 10);
      else return false;
    }
    out = val;
    return true;
  }

  Result<std::string> parse_string() {
    if (pos >= sv.size() || sv[pos] != '"') {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid JSON: expected '\"'"));
    }
    ++pos;
    std::string out;
    while (pos < sv.size()) {
      unsigned char uc = static_cast<unsigned char>(sv[pos++]);
      if (uc == '"') return out;
      if (uc < 0x20u) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "invalid JSON: control char in string"));
      }
      if (uc != '\\') {
        out.push_back(static_cast<char>(uc));
        continue;
      }
      if (pos >= sv.size()) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "invalid JSON: trailing escape"));
      }
      char esc = sv[pos++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!parse_hex4(cp)) {
            return std::unexpected(
                make_err(ErrKind::InvalidData, "invalid JSON: bad \\u escape"));
          }
          if (cp >= 0xD800u && cp <= 0xDBFFu) {
            if (pos + 2 <= sv.size() && sv[pos] == '\\' && sv[pos + 1] == 'u') {
              size_t saved = pos;
              pos += 2;
              uint32_t low = 0;
              if (parse_hex4(low) && low >= 0xDC00u && low <= 0xDFFFu) {
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
              } else {
                pos = saved;
                cp = 0xFFFDu;
              }
            } else {
              cp = 0xFFFDu;
            }
          } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;
          }
          append_utf8(out, cp);
          break;
        }
        default:
          return std::unexpected(
              make_err(ErrKind::InvalidData, "invalid JSON: bad escape sequence"));
      }
    }
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid JSON: unterminated string"));
  }

  Result<Value> parse_number() {
    size_t start = pos;
    if (pos < sv.size() && sv[pos] == '-') ++pos;
    if (pos >= sv.size()) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid JSON: bad number"));
    }
    if (sv[pos] == '0') {
      ++pos;
    } else if (sv[pos] >= '1' && sv[pos] <= '9') {
      while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9') ++pos;
    } else {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid JSON: bad number"));
    }
    if (pos < sv.size() && sv[pos] == '.') {
      ++pos;
      if (pos >= sv.size() || sv[pos] < '0' || sv[pos] > '9') {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "invalid JSON: bad fraction"));
      }
      while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9') ++pos;
    }
    if (pos < sv.size() && (sv[pos] == 'e' || sv[pos] == 'E')) {
      ++pos;
      if (pos < sv.size() && (sv[pos] == '+' || sv[pos] == '-')) ++pos;
      if (pos >= sv.size() || sv[pos] < '0' || sv[pos] > '9') {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "invalid JSON: bad exponent"));
      }
      while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9') ++pos;
    }
    double val = 0.0;
    auto [ptr, ec] = std::from_chars(sv.data() + start, sv.data() + pos, val);
    if (ec != std::errc{} && ec != std::errc::result_out_of_range) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid JSON: number parse failed"));
    }
    Value out;
    out.data = val;
    return out;
  }

  Result<Value> parse_value(int depth) {
    if (depth > 64) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid JSON: max nesting exceeded"));
    }
    skip_ws();
    if (pos >= sv.size()) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid JSON: unexpected EOF"));
    }
    char c = sv[pos];
    if (c == '"') {
      auto s = parse_string();
      if (!s) return std::unexpected(s.error());
      Value out;
      out.data = std::move(*s);
      return out;
    }
    if (c == '{') {
      ++pos;
      auto obj = std::make_unique<Object>();
      skip_ws();
      if (pos < sv.size() && sv[pos] == '}') {
        ++pos;
        Value out;
        out.data = std::move(obj);
        return out;
      }
      while (true) {
        skip_ws();
        auto key = parse_string();
        if (!key) return std::unexpected(key.error());
        skip_ws();
        if (pos >= sv.size() || sv[pos] != ':') {
          return std::unexpected(
              make_err(ErrKind::InvalidData, "invalid JSON: expected ':'"));
        }
        ++pos;
        auto val = parse_value(depth + 1);
        if (!val) return std::unexpected(val.error());
        obj->entries.insert_or_assign(std::move(*key), std::move(*val));
        skip_ws();
        if (pos >= sv.size()) {
          return std::unexpected(
              make_err(ErrKind::InvalidData, "invalid JSON: unterminated object"));
        }
        if (sv[pos] == '}') {
          ++pos;
          Value out;
          out.data = std::move(obj);
          return out;
        }
        if (sv[pos] != ',') {
          return std::unexpected(
              make_err(ErrKind::InvalidData, "invalid JSON: expected ',' or '}'"));
        }
        ++pos;
      }
    }
    if (c == '[') {
      ++pos;
      Value::Array arr;
      skip_ws();
      if (pos < sv.size() && sv[pos] == ']') {
        ++pos;
        Value out;
        out.data = std::move(arr);
        return out;
      }
      while (true) {
        auto item = parse_value(depth + 1);
        if (!item) return std::unexpected(item.error());
        arr.push_back(std::move(*item));
        skip_ws();
        if (pos >= sv.size()) {
          return std::unexpected(
              make_err(ErrKind::InvalidData, "invalid JSON: unterminated array"));
        }
        if (sv[pos] == ']') {
          ++pos;
          Value out;
          out.data = std::move(arr);
          return out;
        }
        if (sv[pos] != ',') {
          return std::unexpected(
              make_err(ErrKind::InvalidData, "invalid JSON: expected ',' or ']'"));
        }
        ++pos;
      }
    }
    if (sv.substr(pos).starts_with("true")) {
      pos += 4;
      Value out;
      out.data = true;
      return out;
    }
    if (sv.substr(pos).starts_with("false")) {
      pos += 5;
      Value out;
      out.data = false;
      return out;
    }
    if (sv.substr(pos).starts_with("null")) {
      pos += 4;
      return Value{};
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return parse_number();
    }
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid JSON: unexpected token"));
  }
};

void append_escaped_str(std::string &out, std::string_view sv) {
  static constexpr char HEX[] = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : sv) {
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (c < 0x20u) {
          out.append("\\u00");
          out.push_back(HEX[(c >> 4) & 0x0Fu]);
          out.push_back(HEX[c & 0x0Fu]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

void serialize_into(std::string &out, const Value &val) {
  if (val.is_null()) {
    out.append("null");
    return;
  }
  if (auto b = val.as_bool()) {
    out.append(*b ? "true" : "false");
    return;
  }
  if (auto d = val.as_num()) {
    if (!std::isfinite(*d)) {
      out.append("null");
      return;
    }
    if (*d == 0.0) {
      out.push_back('0');
      return;
    }
    char buf[64];
    if (*d >= -9007199254740992.0 && *d <= 9007199254740992.0 &&
        std::trunc(*d) == *d) {
      int64_t iv = static_cast<int64_t>(*d);
      auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), iv);
      if (ec == std::errc{}) {
        out.append(buf, static_cast<size_t>(ptr - buf));
        return;
      }
    }
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), *d);
    if (ec == std::errc{}) {
      out.append(buf, static_cast<size_t>(ptr - buf));
    } else {
      out.push_back('0');
    }
    return;
  }
  if (auto s = val.as_str()) {
    append_escaped_str(out, *s);
    return;
  }
  if (const auto *arr = val.as_arr()) {
    out.push_back('[');
    for (size_t i = 0; i < arr->size(); ++i) {
      if (i > 0) out.push_back(',');
      serialize_into(out, (*arr)[i]);
    }
    out.push_back(']');
    return;
  }
  if (const auto *obj = val.as_obj()) {
    out.push_back('{');
    bool first = true;
    for (const auto &[k, v] : obj->entries) {
      if (!first) out.push_back(',');
      first = false;
      append_escaped_str(out, k);
      out.push_back(':');
      serialize_into(out, v);
    }
    out.push_back('}');
  }
}

}  // namespace

Result<Value> parse(std::string_view input) {
  Parser p{input, 0};
  auto res = p.parse_value(0);
  if (!res) return std::unexpected(res.error());
  p.skip_ws();
  if (p.pos != input.size()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid JSON: trailing characters"));
  }
  return res;
}

std::string serialize(const Value &val) {
  std::string out;
  serialize_into(out, val);
  return out;
}

}  // namespace yrs::json

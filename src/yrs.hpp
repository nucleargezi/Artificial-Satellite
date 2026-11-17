#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace yrs {

namespace fs = std::filesystem;

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

inline Result<std::string> read_file_to_string(const fs::path &p) {
  std::FILE *fp = std::fopen(p.c_str(), "rb");
  if (!fp) return std::unexpected(from_errno(p.string()));
  std::string out;
  char buf[8192];
  while (size_t n = std::fread(buf, 1, sizeof(buf), fp)) {
    out.append(buf, n);
  }
  if (std::ferror(fp)) {
    int err = errno;
    std::fclose(fp);
    errno = err;
    return std::unexpected(from_errno(p.string()));
  }
  std::fclose(fp);
  return out;
}

inline Result<void> write_string_to_file(const fs::path &p,
                                         std::string_view content) {
  if (p.empty()) {
    return std::unexpected(make_err(ErrKind::InvalidInput, "empty file path"));
  }

  mode_t mode = 0666;
  bool has_orig_mode = false;
  struct stat st{};
  if (::stat(p.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      errno = EISDIR;
      return std::unexpected(from_errno(p.string()));
    }
    mode = st.st_mode & 07777;
    has_orig_mode = true;
  }

  fs::path parent = p.parent_path();
  if (parent.empty()) parent = ".";
  std::string tmp_tpl = (parent / ".yrs_tmp.XXXXXX").string();

  int fd = ::mkstemp(tmp_tpl.data());
  if (fd < 0) return std::unexpected(from_errno(p.string()));

  auto cleanup_fail = [&](int err) -> Result<void> {
    ::close(fd);
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(p.string()));
  };

  if (has_orig_mode) {
    if (::fchmod(fd, mode) != 0) return cleanup_fail(errno);
  } else {
    mode_t mask = ::umask(0);
    ::umask(mask);
    if (::fchmod(fd, mode & ~mask) != 0) return cleanup_fail(errno);
  }

  const char *ptr = content.data();
  size_t remaining = content.size();
  while (remaining > 0) {
    ssize_t n = ::write(fd, ptr, remaining);
    if (n < 0) {
      if (errno == EINTR) continue;
      return cleanup_fail(errno);
    }
    ptr += n;
    remaining -= static_cast<size_t>(n);
  }

  if (::fsync(fd) != 0 && errno != EINVAL && errno != EROFS) {
    return cleanup_fail(errno);
  }

  if (::close(fd) != 0) {
    int err = errno;
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(p.string()));
  }

  if (::rename(tmp_tpl.c_str(), p.c_str()) != 0) {
    int err = errno;
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(p.string()));
  }

  return {};
}

inline fs::path normalize_path(const fs::path &path) {
  std::error_code ec;
  fs::path norm = fs::weakly_canonical(path, ec);
  if (ec) {
    ec.clear();
    fs::path abs = fs::absolute(path, ec);
    norm = (ec ? path : abs).lexically_normal();
  }
  while (norm.has_relative_path() && norm.filename().empty()) {
    norm = norm.parent_path();
  }
  return norm;
}

inline bool path_starts_with(const fs::path &path, const fs::path &base) {
  auto pit = path.begin();
  for (const auto &b : base) {
    if (b.empty()) continue;
    while (pit != path.end() && pit->empty()) ++pit;
    if (pit == path.end() || *pit != b) return false;
    ++pit;
  }
  return true;
}

struct AppConfig {
  fs::path workspace_root;
  fs::path library_root;
  fs::path catalog_root;
  fs::path record_root;
  fs::path summary_path;
  fs::path template_source;

  static AppConfig defaults_for_dir(const fs::path &dir) {
    return AppConfig{
        .workspace_root = dir,
        .library_root = dir,
        .catalog_root = dir / ".wait",
        .record_root = dir / "record",
        .summary_path = dir / "record" / "summary.md",
        .template_source = dir / "0.cpp",
    };
  }

  static Result<fs::path> restore_summary_path(const fs::path &record_root,
                                               const fs::path &summary_file) {
    auto fail = [&]() -> Result<fs::path> {
      return std::unexpected(
          make_err(ErrKind::InvalidData,
                   "summary_file must be a filename under " +
                       record_root.string()));
    };
    if (summary_file.empty() || summary_file.is_absolute()) return fail();
    auto it = summary_file.begin();
    if (it == summary_file.end()) return fail();
    std::string first = it->string();
    ++it;
    if (it != summary_file.end() || first.empty() || first == "." ||
        first == "..") {
      return fail();
    }
    return record_root / first;
  }

  static Result<std::string> parse_toml_string(std::string_view raw,
                                               const fs::path &config_path) {
    raw = trim(raw);
    if (raw.size() < 2 || raw.front() != '"') {
      return std::unexpected(
          make_err(ErrKind::InvalidData,
                   "invalid " + config_path.string() +
                       ": expected quoted string"));
    }
    std::string out;
    bool escaped = false;
    for (size_t i = 1; i < raw.size(); ++i) {
      char c = raw[i];
      if (escaped) {
        switch (c) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          default:
            return std::unexpected(
                make_err(ErrKind::InvalidData,
                         "invalid " + config_path.string() +
                             ": unsupported escape sequence"));
        }
        escaped = false;
        continue;
      }
      if (c == '\\') {
        escaped = true;
        continue;
      }
      if (c == '"') {
        std::string_view rest = trim(raw.substr(i + 1));
        if (!rest.empty() && rest.front() != '#') {
          return std::unexpected(
              make_err(ErrKind::InvalidData,
                       "invalid " + config_path.string() +
                           ": trailing characters after string"));
        }
        return out;
      }
      out.push_back(c);
    }
    return std::unexpected(
        make_err(ErrKind::InvalidData,
                 "invalid " + config_path.string() + ": unterminated string"));
  }

  static Result<AppConfig> load_for_dir(const fs::path &dir) {
    AppConfig config = defaults_for_dir(dir);
    fs::path config_path = dir / "yrs.toml";
    if (!fs::exists(config_path)) {
      return std::unexpected(
          make_err(ErrKind::NotFound, "missing " + config_path.string()));
    }

    auto content_res = read_file_to_string(config_path);
    if (!content_res) return std::unexpected(content_res.error());

    std::map<std::string, std::string> kv;
    std::string_view sv = *content_res;
    size_t pos = 0;
    while (pos < sv.size()) {
      size_t nl = sv.find('\n', pos);
      std::string_view raw_line =
          (nl == std::string_view::npos) ? sv.substr(pos)
                                         : sv.substr(pos, nl - pos);
      if (nl == std::string_view::npos) pos = sv.size();
      else pos = nl + 1;

      std::string_view line = trim(raw_line);
      if (line.empty() || line.front() == '#') continue;
      auto eq = line.find('=');
      if (eq == std::string_view::npos) {
        return std::unexpected(
            make_err(ErrKind::InvalidData,
                     "invalid " + config_path.string() +
                         ": expected key = value"));
      }
      std::string key(trim(line.substr(0, eq)));
      std::string_view val_part = line.substr(eq + 1);
      if (key != "library_root" && key != "catalog_root" &&
          key != "record_root" && key != "summary_file" &&
          key != "template_source") {
        return std::unexpected(
            make_err(ErrKind::InvalidData,
                     "invalid " + config_path.string() + ": unknown field `" +
                         key + "`"));
      }
      if (kv.contains(key)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData,
                     "invalid " + config_path.string() + ": duplicate field `" +
                         key + "`"));
      }
      auto parsed = parse_toml_string(val_part, config_path);
      if (!parsed) return std::unexpected(parsed.error());
      kv.emplace(std::move(key), std::move(*parsed));
    }

    for (const char *req :
         {"library_root", "catalog_root", "record_root", "summary_file"}) {
      if (!kv.contains(req)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData,
                     "invalid " + config_path.string() + ": missing field `" +
                         req + "`"));
      }
    }

    fs::path lib_root = kv["library_root"];
    if (!lib_root.is_absolute()) {
      return std::unexpected(make_err(
          ErrKind::InvalidData, "library_root must be an absolute path"));
    }
    config.library_root = lib_root;

    auto restore_path = [&](const fs::path &p) -> fs::path {
      return p.is_absolute() ? p : (config.workspace_root / p);
    };

    if (kv.contains("template_source")) {
      config.template_source = restore_path(kv["template_source"]);
    }
    config.catalog_root = restore_path(kv["catalog_root"]);
    config.record_root = restore_path(kv["record_root"]);

    auto summary_res =
        restore_summary_path(config.record_root, kv["summary_file"]);
    if (!summary_res) return std::unexpected(summary_res.error());
    config.summary_path = std::move(*summary_res);

    return config;
  }
};

// --- Latest ---

inline Result<std::optional<fs::path>> find_latest_cpp(const fs::path &dir) {
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));

  std::optional<std::pair<std::pair<int64_t, int64_t>, fs::path>> best;
  for (const auto &entry : it) {
    auto st = entry.symlink_status(ec);
    if (ec || st.type() != fs::file_type::regular) continue;

    const fs::path &p = entry.path();
    if (p.extension() != ".cpp") continue;

    struct stat sb{};
    if (::stat(p.c_str(), &sb) != 0) {
      return std::unexpected(from_errno(p.string()));
    }
    std::pair<int64_t, int64_t> mtime{sb.st_mtim.tv_sec, sb.st_mtim.tv_nsec};
    if (!best || best->first < mtime) {
      best = std::make_pair(mtime, p);
    }
  }
  if (!best) return std::nullopt;
  return best->second;
}

inline Result<fs::path> cover_latest_cpp(const AppConfig &config) {
  std::error_code ec;
  if (!fs::is_regular_file(config.template_source, ec)) {
    return std::unexpected(
        make_err(ErrKind::NotFound,
                 "no default file: " + config.template_source.string()));
  }

  auto tpl_content = read_file_to_string(config.template_source);
  if (!tpl_content) return std::unexpected(tpl_content.error());

  auto latest_res = find_latest_cpp(config.workspace_root);
  if (!latest_res) return std::unexpected(latest_res.error());
  if (!latest_res->has_value()) {
    return std::unexpected(
        make_err(ErrKind::NotFound,
                 "no cpp file in " + config.workspace_root.string()));
  }

  const fs::path &target = **latest_res;
  auto write_res = write_string_to_file(target, *tpl_content);
  if (!write_res) return std::unexpected(write_res.error());

  return target;
}

// --- Bundle ---

struct BundledLatest {
  fs::path source_path;
  std::string content;
  std::vector<fs::path> visited_local_files;
};

namespace detail {

struct VisitedFiles {
  std::set<fs::path> seen;
  std::vector<fs::path> ordered;

  bool insert(const fs::path &p) {
    if (seen.insert(p).second) {
      ordered.push_back(p);
      return true;
    }
    return false;
  }
};

inline std::string strip_line_comment(std::string_view line) {
  std::string out;
  out.reserve(line.size());
  bool in_single = false;
  bool in_double = false;
  bool escaped = false;

  for (size_t i = 0; i < line.size(); ++i) {
    char ch = line[i];
    if (escaped) {
      out.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\' && (in_single || in_double)) {
      out.push_back(ch);
      escaped = true;
      continue;
    }
    if (ch == '\'' && !in_double) {
      in_single = !in_single;
      out.push_back(ch);
      continue;
    }
    if (ch == '"' && !in_single) {
      in_double = !in_double;
      out.push_back(ch);
      continue;
    }
    if (ch == '/' && !in_single && !in_double && i + 1 < line.size() &&
        line[i + 1] == '/') {
      break;
    }
    out.push_back(ch);
  }
  return out;
}

inline std::optional<std::string_view> parse_local_include(
    std::string_view line) {
  if (!line.starts_with("#include")) return std::nullopt;
  std::string_view rest = trim_start(line.substr(8));
  if (rest.empty() || rest.front() != '"') return std::nullopt;
  rest.remove_prefix(1);
  auto end = rest.find('"');
  if (end == std::string_view::npos) return std::nullopt;
  if (!trim(rest.substr(end + 1)).empty()) return std::nullopt;
  return rest.substr(0, end);
}

inline std::optional<fs::path> resolve_include_path(
    const fs::path &current_file, const fs::path &library_root,
    std::string_view include_path) {
  std::error_code ec;
  if (current_file.has_parent_path()) {
    fs::path resolved =
        normalize_path(current_file.parent_path() / include_path);
    if (path_starts_with(resolved, library_root) &&
        fs::is_regular_file(resolved, ec)) {
      return resolved;
    }
  }
  fs::path resolved = normalize_path(library_root / include_path);
  if (!path_starts_with(resolved, library_root) ||
      !fs::is_regular_file(resolved, ec)) {
    return std::nullopt;
  }
  return resolved;
}

inline Result<std::vector<std::string>> expand_file(const fs::path &path,
                                                    const fs::path &library_root,
                                                    VisitedFiles &visited) {
  auto content_res = read_file_to_string(path);
  if (!content_res) return std::unexpected(content_res.error());

  std::vector<std::string> lines;
  std::string_view sv = *content_res;
  size_t pos = 0;
  while (pos < sv.size()) {
    size_t next_nl = sv.find('\n', pos);
    std::string_view raw_line =
        (next_nl == std::string_view::npos) ? sv.substr(pos)
                                            : sv.substr(pos, next_nl - pos);
    if (!raw_line.empty() && raw_line.back() == '\r') {
      raw_line.remove_suffix(1);
    }
    std::string stripped = strip_line_comment(raw_line);
    std::string line(trim_end(stripped));
    std::string_view trimmed = trim_start(line);

    if (trimmed != "#pragma once") {
      bool expanded = false;
      if (auto inc = parse_local_include(trimmed)) {
        if (auto inc_file = resolve_include_path(path, library_root, *inc)) {
          if (visited.insert(*inc_file)) {
            auto sub = expand_file(*inc_file, library_root, visited);
            if (!sub) return std::unexpected(sub.error());
            lines.insert(lines.end(),
                         std::make_move_iterator(sub->begin()),
                         std::make_move_iterator(sub->end()));
          }
          expanded = true;
        }
      }
      if (!expanded) {
        lines.push_back(std::move(line));
      }
    }

    if (next_nl == std::string_view::npos) break;
    pos = next_nl + 1;
  }
  return lines;
}

inline std::string normalize_lines(std::vector<std::string> lines) {
  std::vector<std::string> normalized;
  bool prev_blank = true;
  for (auto &line : lines) {
    bool is_blank = line.empty();
    if (is_blank && prev_blank) continue;
    prev_blank = is_blank;
    normalized.push_back(std::move(line));
  }
  while (!normalized.empty() && normalized.back().empty()) {
    normalized.pop_back();
  }
  std::string out;
  for (size_t i = 0; i < normalized.size(); ++i) {
    if (i > 0) out.push_back('\n');
    out.append(normalized[i]);
  }
  out.push_back('\n');
  return out;
}

}  // namespace detail

inline Result<BundledLatest> bundle_cpp_path(const AppConfig &config,
                                             const fs::path &source_path) {
  fs::path library_root = normalize_path(config.library_root);
  fs::path norm_source = normalize_path(source_path);
  detail::VisitedFiles visited;
  auto lines_res = detail::expand_file(norm_source, library_root, visited);
  if (!lines_res) return std::unexpected(lines_res.error());

  return BundledLatest{
      .source_path = norm_source,
      .content = detail::normalize_lines(std::move(*lines_res)),
      .visited_local_files = std::move(visited.ordered),
  };
}

inline Result<BundledLatest> bundle_latest_cpp(const AppConfig &config) {
  auto latest_res = find_latest_cpp(config.workspace_root);
  if (!latest_res) return std::unexpected(latest_res.error());
  if (!latest_res->has_value()) {
    return std::unexpected(
        make_err(ErrKind::NotFound,
                 "no cpp file in " + config.workspace_root.string()));
  }
  return bundle_cpp_path(config, **latest_res);
}

// --- Move & History ---

enum class MoveEntryStatus {
  Created,
  Overwritten,
};

struct MoveEntry {
  fs::path relative_path;
  MoveEntryStatus status;
  int64_t source_modified_sec;
};

struct MoveSummary {
  std::vector<MoveEntry> entries;
};

namespace detail {

inline Result<void> copy_file_atomic(const fs::path &from, const fs::path &to) {
  int src_fd = ::open(from.c_str(), O_RDONLY);
  if (src_fd < 0) return std::unexpected(from_errno(from.string()));

  struct stat sb{};
  if (::fstat(src_fd, &sb) != 0) {
    int err = errno;
    ::close(src_fd);
    errno = err;
    return std::unexpected(from_errno(from.string()));
  }

  fs::path parent = to.parent_path();
  if (parent.empty()) parent = ".";
  std::string tmp_tpl = (parent / ".yrs_tmp.XXXXXX").string();

  int dst_fd = ::mkstemp(tmp_tpl.data());
  if (dst_fd < 0) {
    int err = errno;
    ::close(src_fd);
    errno = err;
    return std::unexpected(from_errno(to.string()));
  }

  auto cleanup_fail = [&](int err, const fs::path &ctx) -> Result<void> {
    ::close(src_fd);
    ::close(dst_fd);
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(ctx.string()));
  };

  if (::fchmod(dst_fd, sb.st_mode & 07777) != 0) {
    return cleanup_fail(errno, to);
  }

  char buf[16384];
  while (true) {
    ssize_t nread = ::read(src_fd, buf, sizeof(buf));
    if (nread < 0) {
      if (errno == EINTR) continue;
      return cleanup_fail(errno, from);
    }
    if (nread == 0) break;

    const char *ptr = buf;
    size_t remaining = static_cast<size_t>(nread);
    while (remaining > 0) {
      ssize_t nwritten = ::write(dst_fd, ptr, remaining);
      if (nwritten < 0) {
        if (errno == EINTR) continue;
        return cleanup_fail(errno, to);
      }
      ptr += nwritten;
      remaining -= static_cast<size_t>(nwritten);
    }
  }

  struct timespec ts[2] = {sb.st_atim, sb.st_mtim};
  (void)::futimens(dst_fd, ts);

  if (::fsync(dst_fd) != 0 && errno != EINVAL && errno != EROFS) {
    return cleanup_fail(errno, to);
  }

  ::close(src_fd);
  if (::close(dst_fd) != 0) {
    int err = errno;
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(to.string()));
  }

  if (::rename(tmp_tpl.c_str(), to.c_str()) != 0) {
    int err = errno;
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(to.string()));
  }

  return {};
}

inline Result<void> move_file_across(const fs::path &from, const fs::path &to) {
  if (::rename(from.c_str(), to.c_str()) == 0) {
    return {};
  }
  if (errno == EXDEV) {
    auto cp_res = copy_file_atomic(from, to);
    if (!cp_res) return cp_res;
    if (::unlink(from.c_str()) != 0) {
      return std::unexpected(from_errno(from.string()));
    }
    return {};
  }
  return std::unexpected(from_errno(from.string()));
}

inline Result<std::vector<MoveEntry>> move_dir_rec(
    const fs::path &cur_dir, const fs::path &catalog_root,
    const fs::path &record_root, const fs::path &summary_path) {
  std::error_code ec;
  std::vector<fs::directory_entry> dir_entries;
  for (const auto &entry : fs::directory_iterator(cur_dir, ec)) {
    if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
    dir_entries.push_back(entry);
  }
  std::sort(dir_entries.begin(), dir_entries.end(),
            [](const auto &a, const auto &b) { return a.path() < b.path(); });

  std::vector<MoveEntry> moved;
  for (const auto &dir_entry : dir_entries) {
    auto st = dir_entry.symlink_status(ec);
    if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));

    const fs::path &source_path = dir_entry.path();
    fs::path relative_path = source_path.lexically_relative(catalog_root);
    fs::path destination_path = record_root / relative_path;

    if (destination_path == summary_path) {
      return std::unexpected(make_err(
          ErrKind::InvalidInput,
          "target path is reserved for summary file: " +
              destination_path.string()));
    }

    if (st.type() == fs::file_type::directory) {
      fs::create_directories(destination_path, ec);
      if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
      auto sub = move_dir_rec(source_path, catalog_root, record_root,
                              summary_path);
      if (!sub) return std::unexpected(sub.error());
      moved.insert(moved.end(), std::make_move_iterator(sub->begin()),
                   std::make_move_iterator(sub->end()));
      continue;
    }

    if (st.type() != fs::file_type::regular) continue;

    struct stat sb{};
    if (::stat(source_path.c_str(), &sb) != 0) {
      return std::unexpected(from_errno(source_path.string()));
    }
    int64_t mtime_sec = sb.st_mtim.tv_sec;

    if (destination_path.has_parent_path()) {
      fs::create_directories(destination_path.parent_path(), ec);
      if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
    }

    MoveEntryStatus status = MoveEntryStatus::Created;
    auto dst_st = fs::symlink_status(destination_path, ec);
    if (dst_st.type() != fs::file_type::not_found) {
      if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
      if (dst_st.type() == fs::file_type::directory) {
        return std::unexpected(make_err(
            ErrKind::InvalidInput,
            "destination is an existing directory: " +
                destination_path.string()));
      }
      status = MoveEntryStatus::Overwritten;
    } else {
      ec.clear();
    }

    auto mv_res = move_file_across(source_path, destination_path);
    if (!mv_res) return std::unexpected(mv_res.error());

    moved.push_back(MoveEntry{
        .relative_path = std::move(relative_path),
        .status = status,
        .source_modified_sec = mtime_sec,
    });
  }
  return moved;
}

}  // namespace detail

inline Result<MoveSummary> move_catalog_into_record(const AppConfig &config) {
  std::error_code ec;
  if (!fs::exists(config.catalog_root, ec)) {
    return std::unexpected(
        make_err(ErrKind::NotFound,
                 "missing catalog root: " + config.catalog_root.string()));
  }
  if (!fs::is_directory(config.catalog_root, ec)) {
    return std::unexpected(
        make_err(ErrKind::InvalidInput,
                 "catalog root is not a dir: " + config.catalog_root.string()));
  }

  fs::create_directories(config.record_root, ec);
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));

  auto entries_res = detail::move_dir_rec(
      config.catalog_root, config.catalog_root, config.record_root,
      config.summary_path);
  if (!entries_res) return std::unexpected(entries_res.error());

  auto entries = std::move(*entries_res);
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    return a.relative_path < b.relative_path;
  });
  return MoveSummary{std::move(entries)};
}

struct MoveDate {
  int64_t year;
  uint32_t month;
  uint32_t day;

  auto operator<=>(const MoveDate &) const = default;

  static MoveDate from_unix_seconds(int64_t unix_seconds) {
    int64_t shifted = unix_seconds + 8 * 3600;
    int64_t days = shifted >= 0 ? (shifted / 86400)
                                : ((shifted - 86399) / 86400);
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2 ? 1 : 0);
    return MoveDate{y, static_cast<uint32_t>(m), static_cast<uint32_t>(d)};
  }

  std::string format(char sep) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld%c%02u%c%02u",
                  static_cast<long long>(year), sep, month, sep, day);
    return buf;
  }
};

struct MoveTree {
  std::map<std::string, MoveTree> dirs;
  std::map<std::string, MoveEntryStatus> files;

  void insert(const fs::path &relative_path, MoveEntryStatus status) {
    MoveTree *current = this;
    std::vector<std::string> parts;
    for (const auto &comp : relative_path) {
      if (!comp.empty()) parts.push_back(comp.string());
    }
    if (parts.empty()) return;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      current = &current->dirs[parts[i]];
    }
    current->files[parts.back()] = status;
  }
};

struct MoveDayGroup {
  MoveDate date;
  MoveTree tree;
};

inline std::vector<MoveDayGroup> build_move_day_groups(
    const std::vector<MoveEntry> &entries) {
  std::map<MoveDate, MoveTree> grouped;
  for (const auto &entry : entries) {
    MoveDate date = MoveDate::from_unix_seconds(entry.source_modified_sec);
    grouped[date].insert(entry.relative_path, entry.status);
  }
  std::vector<MoveDayGroup> out;
  out.reserve(grouped.size());
  for (auto it = grouped.rbegin(); it != grouped.rend(); ++it) {
    out.push_back(MoveDayGroup{it->first, std::move(it->second)});
  }
  return out;
}

namespace render {

inline constexpr std::string_view ANSI_BLUE = "\x1b[94m";
inline constexpr std::string_view ANSI_GREEN = "\x1b[32m";
inline constexpr std::string_view ANSI_RESET = "\x1b[0m";

inline std::string_view branch(bool is_last) {
  return is_last ? "└── " : "├── ";
}

inline std::string child_prefix(std::string_view prefix, bool is_last) {
  std::string s(prefix);
  s.append(is_last ? "    " : "│   ");
  return s;
}

inline void render_terminal_dir(const MoveTree &dir, std::string_view prefix,
                                std::vector<std::string> &lines) {
  size_t total = dir.dirs.size() + dir.files.size();
  size_t idx = 0;

  for (const auto &[name, child_dir] : dir.dirs) {
    ++idx;
    bool is_last = (idx == total);
    std::string line(prefix);
    line.append(branch(is_last)).append(name);
    lines.push_back(std::move(line));
    render_terminal_dir(child_dir, child_prefix(prefix, is_last), lines);
  }

  for (const auto &[name, status] : dir.files) {
    ++idx;
    bool is_last = (idx == total);
    std::string_view color =
        (status == MoveEntryStatus::Created) ? ANSI_GREEN : ANSI_BLUE;
    std::string line(prefix);
    line.append(branch(is_last)).append(color).append(name).append(ANSI_RESET);
    lines.push_back(std::move(line));
  }
}

inline std::string render_move_tree(const fs::path &record_root,
                                    const MoveTree &tree) {
  std::string root_name = record_root.filename().empty()
                              ? record_root.string()
                              : record_root.filename().string();
  std::vector<std::string> lines{std::move(root_name)};
  render_terminal_dir(tree, "", lines);
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) out.push_back('\n');
    out.append(lines[i]);
  }
  return out;
}

inline std::string render_grouped_move_trees(
    const fs::path &record_root, const std::vector<MoveDayGroup> &groups) {
  std::string out;
  for (size_t i = 0; i < groups.size(); ++i) {
    if (i > 0) out.append("\n\n");
    out.append(groups[i].date.format('-'));
    out.push_back('\n');
    out.append(render_move_tree(record_root, groups[i].tree));
  }
  return out;
}

inline void render_summary_dir(const MoveTree &dir, std::string_view prefix,
                               std::vector<std::string> &lines) {
  size_t total = dir.dirs.size() + (dir.files.empty() ? 0 : 1);
  size_t idx = 0;

  for (const auto &[name, child_dir] : dir.dirs) {
    ++idx;
    bool is_last = (idx == total);
    std::string line(prefix);
    line.append(branch(is_last)).append(name);
    lines.push_back(std::move(line));
    render_summary_dir(child_dir, child_prefix(prefix, is_last), lines);
  }

  if (!dir.files.empty()) {
    ++idx;
    bool is_last = (idx == total);
    std::string line(prefix);
    line.append(branch(is_last));
    bool first = true;
    for (const auto &[name, status] : dir.files) {
      if (!first) line.append(" | ");
      first = false;
      line.append(name);
      if (status == MoveEntryStatus::Overwritten) {
        line.append(" [cover]");
      }
    }
    lines.push_back(std::move(line));
  }
}

inline std::string render_summary_tree(const MoveTree &tree) {
  std::vector<std::string> raw_lines;
  render_summary_dir(tree, "", raw_lines);
  std::string out;
  for (size_t i = 0; i < raw_lines.size(); ++i) {
    if (i > 0) out.push_back('\n');
    out.append("  ").append(raw_lines[i]);
  }
  return out;
}

inline std::string render_summary_history_blocks(
    const std::vector<MoveDayGroup> &groups) {
  std::string out;
  for (const auto &group : groups) {
    out.append("- ")
        .append(group.date.format('/'))
        .append("\n  ```sh\n")
        .append(render_summary_tree(group.tree))
        .append("\n  ```\n");
  }
  return out;
}

}  // namespace render

inline constexpr std::string_view HISTORY_HEADER_PREFIX =
    "#### History (since ";

inline Result<void> update_summary_file(
    const AppConfig &config, const std::vector<MoveDayGroup> &groups) {
  if (groups.empty()) return {};

  std::string new_blocks = render::render_summary_history_blocks(groups);
  if (fs::exists(config.summary_path)) {
    auto old_res = read_file_to_string(config.summary_path);
    if (!old_res) return std::unexpected(old_res.error());
    const std::string &existing = *old_res;
    if (!existing.starts_with(HISTORY_HEADER_PREFIX)) {
      return std::unexpected(make_err(
          ErrKind::InvalidData, "summary file is missing the History heading"));
    }
    auto first_nl = existing.find('\n');
    std::string updated;
    if (first_nl == std::string::npos) {
      updated.append(existing).push_back('\n');
      updated.append(new_blocks);
    } else {
      updated.append(existing, 0, first_nl + 1);
      updated.append(new_blocks);
      updated.append(existing, first_nl + 1, std::string::npos);
    }
    return write_string_to_file(config.summary_path, updated);
  }

  int64_t now_sec = static_cast<int64_t>(std::time(nullptr));
  std::string since = MoveDate::from_unix_seconds(now_sec).format('/');
  std::string content(HISTORY_HEADER_PREFIX);
  content.append(since).append(")\n").append(new_blocks);
  return write_string_to_file(config.summary_path, content);
}

// --- Clipboard ---

namespace clipboard {

inline bool has_env(const char *name) {
  const char *val = std::getenv(name);
  return val != nullptr && val[0] != '\0';
}

inline Result<void> run_clipboard_command(
    const char *program, const std::vector<const char *> &args,
    std::string_view content) {
  int stdin_pipe[2];
  if (::pipe(stdin_pipe) != 0) return std::unexpected(from_errno("pipe"));

  int err_pipe[2];
  if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
    ::close(stdin_pipe[0]);
    ::close(stdin_pipe[1]);
    return std::unexpected(from_errno("pipe2"));
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(stdin_pipe[0]);
    ::close(stdin_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    return std::unexpected(from_errno("fork"));
  }

  if (pid == 0) {
    ::close(stdin_pipe[1]);
    ::close(err_pipe[0]);

    if (::dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
      int err = errno;
      if (::write(err_pipe[1], &err, sizeof(err)) < 0) {}
      ::_exit(127);
    }
    ::close(stdin_pipe[0]);

    int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::close(devnull);
    }

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(program));
    for (const char *a : args) argv.push_back(const_cast<char *>(a));
    argv.push_back(nullptr);

    std::signal(SIGPIPE, SIG_DFL);
    ::execvp(program, argv.data());
    int err = errno;
    if (::write(err_pipe[1], &err, sizeof(err)) < 0) {}
    ::_exit(127);
  }

  ::close(stdin_pipe[0]);
  ::close(err_pipe[1]);

  int child_errno = 0;
  ssize_t nread = ::read(err_pipe[0], &child_errno, sizeof(child_errno));
  ::close(err_pipe[0]);

  if (nread > 0) {
    ::close(stdin_pipe[1]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ErrKind k = (child_errno == ENOENT) ? ErrKind::NotFound : ErrKind::Other;
    return std::unexpected(make_err(k, std::strerror(child_errno)));
  }

  struct sigaction ign_sa{}, old_sa{};
  ign_sa.sa_handler = SIG_IGN;
  sigemptyset(&ign_sa.sa_mask);
  bool sig_guarded = (::sigaction(SIGPIPE, &ign_sa, &old_sa) == 0);
  struct SigpipeGuard {
    bool active;
    struct sigaction *old_act;
    ~SigpipeGuard() {
      if (active) ::sigaction(SIGPIPE, old_act, nullptr);
    }
  } sig_guard{sig_guarded, &old_sa};

  const char *ptr = content.data();
  size_t remaining = content.size();
  while (remaining > 0) {
    ssize_t written = ::write(stdin_pipe[1], ptr, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      int err = errno;
      ::close(stdin_pipe[1]);
      int status = 0;
      if (::waitpid(pid, &status, 0) >= 0 && err == EPIPE &&
          (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        std::string msg = "clipboard command `";
        msg.append(program).append("` failed (exit status: ");
        msg.append(
            std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1));
        msg.append("); see stderr above");
        return std::unexpected(make_err(ErrKind::Other, std::move(msg)));
      }
      return std::unexpected(make_err(ErrKind::Other, std::strerror(err)));
    }
    ptr += written;
    remaining -= static_cast<size_t>(written);
  }
  ::close(stdin_pipe[1]);

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    return std::unexpected(from_errno("waitpid"));
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return {};
  }
  std::string msg = "clipboard command `";
  msg.append(program).append("` failed (exit status: ");
  msg.append(std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1));
  msg.append("); see stderr above");
  return std::unexpected(make_err(ErrKind::Other, std::move(msg)));
}

inline Result<void> copy_to_clipboard(std::string_view content) {
  struct Backend {
    const char *program;
    std::vector<const char *> args;
  };
  std::vector<Backend> backends;

  bool is_wsl = has_env("WSL_DISTRO_NAME") || has_env("WSL_INTEROP");
  if (!is_wsl) {
    if (auto rel = read_file_to_string("/proc/sys/kernel/osrelease")) {
      std::string lower = *rel;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (lower.find("microsoft") != std::string::npos) is_wsl = true;
    }
  }

  if (is_wsl) {
    backends.push_back({"clip.exe", {}});
  }
  if (has_env("WAYLAND_DISPLAY")) {
    backends.push_back({"wl-copy", {"--type", "text/plain;charset=utf-8"}});
  }
  if (has_env("DISPLAY")) {
    backends.push_back({"xclip", {"-selection", "clipboard"}});
    backends.push_back({"xsel", {"--clipboard", "--input"}});
  }

  for (const auto &b : backends) {
    auto res = run_clipboard_command(b.program, b.args, content);
    if (res) return {};
    if (res.error().kind == ErrKind::NotFound) continue;
    return std::unexpected(res.error());
  }

  return std::unexpected(make_err(
      ErrKind::NotFound,
      "no clipboard backend available: Wayland requires wl-copy (Ubuntu: sudo "
      "apt install wl-clipboard); X11 requires xclip or xsel; Windows/WSL "
      "requires clip.exe. Run in a desktop session with WAYLAND_DISPLAY or "
      "DISPLAY set, or enable WSL Windows interoperability."));
}

}  // namespace clipboard

// --- Gzip (RFC 1952 + RFC 1951) ---

namespace gzip {

namespace detail {

inline uint32_t crc32_ieee(std::string_view data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char b : data) {
    crc ^= b;
    for (int i = 0; i < 8; ++i) {
      uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

inline uint16_t read_le16(const unsigned char *p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_le32(const unsigned char *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

struct BitReader {
  const unsigned char *data;
  size_t pos;
  size_t end;
  uint32_t bit_buf = 0;
  int bit_cnt = 0;

  bool ensure(int n) {
    while (bit_cnt < n) {
      if (pos >= end) return false;
      bit_buf |= static_cast<uint32_t>(data[pos++]) << bit_cnt;
      bit_cnt += 8;
    }
    return true;
  }

  bool read_bits(int n, uint32_t &out) {
    if (n == 0) {
      out = 0;
      return true;
    }
    if (!ensure(n)) return false;
    out = bit_buf & ((1u << n) - 1u);
    bit_buf >>= n;
    bit_cnt -= n;
    return true;
  }

  void align_to_byte() {
    int discard = bit_cnt & 7;
    bit_buf >>= discard;
    bit_cnt -= discard;
    pos -= static_cast<size_t>(bit_cnt / 8);
    bit_buf = 0;
    bit_cnt = 0;
  }

  bool read_aligned_u16(uint16_t &out) {
    if (pos + 2 > end) return false;
    out = read_le16(data + pos);
    pos += 2;
    return true;
  }
};

struct HuffTree {
  std::array<uint16_t, 16> count{};
  std::vector<uint16_t> symbol;

  bool build(std::span<const uint8_t> lengths) {
    count.fill(0);
    for (uint8_t len : lengths) {
      if (len > 15) return false;
      ++count[len];
    }

    if (count[0] == lengths.size()) {
      symbol.clear();
      return true;
    }

    int left = 1;
    for (int len = 1; len <= 15; ++len) {
      left <<= 1;
      left -= count[len];
      if (left < 0) return false;
    }

    std::array<uint16_t, 16> offs{};
    for (int len = 1; len < 15; ++len) {
      offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
    }

    symbol.assign(offs[15] + count[15], 0);
    for (size_t sym = 0; sym < lengths.size(); ++sym) {
      uint8_t len = lengths[sym];
      if (len != 0) {
        symbol[offs[len]++] = static_cast<uint16_t>(sym);
      }
    }
    return true;
  }

  bool decode(BitReader &br, uint16_t &out_sym) const {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 15; ++len) {
      uint32_t bit = 0;
      if (!br.read_bits(1, bit)) return false;
      code |= static_cast<int>(bit);
      int cnt = count[len];
      if (code - cnt < first) {
        out_sym = symbol[static_cast<size_t>(index + (code - first))];
        return true;
      }
      index += cnt;
      first += cnt;
      first <<= 1;
      code <<= 1;
    }
    return false;
  }
};

inline Result<void> decode_huffman_block(BitReader &br, const HuffTree &lit_tree,
                                         const HuffTree &dist_tree,
                                         std::string &out) {
  static constexpr std::array<uint16_t, 29> LEN_BASE = {
      3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static constexpr std::array<uint8_t, 29> LEN_EXTRA = {
      0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static constexpr std::array<uint16_t, 30> DIST_BASE = {
      1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
      33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static constexpr std::array<uint8_t, 30> DIST_EXTRA = {
      0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  static constexpr size_t MAX_OUTPUT_BYTES = 128ULL * 1024ULL * 1024ULL;

  while (true) {
    uint16_t sym = 0;
    if (!lit_tree.decode(br, sym)) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid deflate literal symbol"));
    }
    if (sym < 256) {
      if (out.size() >= MAX_OUTPUT_BYTES) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "gzip output exceeds size limit"));
      }
      out.push_back(static_cast<char>(sym));
      continue;
    }
    if (sym == 256) return {};
    if (sym >= 286) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid deflate length symbol"));
    }

    size_t lidx = sym - 257;
    uint32_t extra = 0;
    if (!br.read_bits(LEN_EXTRA[lidx], extra)) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated deflate length extra bits"));
    }
    size_t match_len = LEN_BASE[lidx] + extra;

    uint16_t dsym = 0;
    if (!dist_tree.decode(br, dsym) || dsym >= 30) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid deflate distance symbol"));
    }
    uint32_t dextra = 0;
    if (!br.read_bits(DIST_EXTRA[dsym], dextra)) {
      return std::unexpected(make_err(ErrKind::InvalidData,
                                      "truncated deflate distance extra bits"));
    }
    size_t dist = DIST_BASE[dsym] + dextra;
    if (dist == 0 || dist > out.size()) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid deflate back-reference"));
    }
    if (out.size() + match_len > MAX_OUTPUT_BYTES) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "gzip output exceeds size limit"));
    }

    for (size_t i = 0; i < match_len; ++i) {
      out.push_back(out[out.size() - dist]);
    }
  }
}

inline Result<void> decode_fixed_block(BitReader &br, std::string &out) {
  std::array<uint8_t, 288> lit_lens{};
  for (size_t i = 0; i <= 143; ++i) lit_lens[i] = 8;
  for (size_t i = 144; i <= 255; ++i) lit_lens[i] = 9;
  for (size_t i = 256; i <= 279; ++i) lit_lens[i] = 7;
  for (size_t i = 280; i <= 287; ++i) lit_lens[i] = 8;

  std::array<uint8_t, 32> dist_lens{};
  dist_lens.fill(5);

  HuffTree lit_tree;
  HuffTree dist_tree;
  if (!lit_tree.build(lit_lens) || !dist_tree.build(dist_lens)) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "failed to build fixed huffman trees"));
  }
  return decode_huffman_block(br, lit_tree, dist_tree, out);
}

inline Result<void> decode_dynamic_block(BitReader &br, std::string &out) {
  static constexpr std::array<uint8_t, 19> CL_ORDER = {
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

  uint32_t hlit_raw = 0, hdist_raw = 0, hclen_raw = 0;
  if (!br.read_bits(5, hlit_raw) || !br.read_bits(5, hdist_raw) ||
      !br.read_bits(4, hclen_raw)) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "truncated dynamic deflate header"));
  }
  size_t hlit = hlit_raw + 257;
  size_t hdist = hdist_raw + 1;
  size_t hclen = hclen_raw + 4;
  if (hlit > 286 || hdist > 30) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid dynamic deflate code counts"));
  }

  std::array<uint8_t, 19> cl_lens{};
  for (size_t i = 0; i < hclen; ++i) {
    uint32_t v = 0;
    if (!br.read_bits(3, v)) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated code length table"));
    }
    cl_lens[CL_ORDER[i]] = static_cast<uint8_t>(v);
  }

  HuffTree cl_tree;
  if (!cl_tree.build(cl_lens)) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid code length huffman tree"));
  }

  std::vector<uint8_t> lens(hlit + hdist, 0);
  size_t idx = 0;
  while (idx < hlit + hdist) {
    uint16_t sym = 0;
    if (!cl_tree.decode(br, sym)) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid code length symbol"));
    }
    if (sym < 16) {
      lens[idx++] = static_cast<uint8_t>(sym);
      continue;
    }

    uint8_t rep_val = 0;
    size_t rep_cnt = 0;
    uint32_t extra = 0;
    if (sym == 16) {
      if (idx == 0 || !br.read_bits(2, extra)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "invalid repeat length code"));
      }
      rep_val = lens[idx - 1];
      rep_cnt = 3 + extra;
    } else if (sym == 17) {
      if (!br.read_bits(3, extra)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "invalid repeat zero code"));
      }
      rep_cnt = 3 + extra;
    } else if (sym == 18) {
      if (!br.read_bits(7, extra)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "invalid repeat long zero code"));
      }
      rep_cnt = 11 + extra;
    } else {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "unexpected code length symbol"));
    }

    if (idx + rep_cnt > hlit + hdist) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "code length table overflow"));
    }
    for (size_t k = 0; k < rep_cnt; ++k) {
      lens[idx++] = rep_val;
    }
  }

  if (lens[256] == 0) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "missing end-of-block symbol"));
  }

  HuffTree lit_tree;
  HuffTree dist_tree;
  if (!lit_tree.build(std::span<const uint8_t>(lens.data(), hlit)) ||
      !dist_tree.build(std::span<const uint8_t>(lens.data() + hlit, hdist))) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid dynamic huffman tree"));
  }
  return decode_huffman_block(br, lit_tree, dist_tree, out);
}

}  // namespace detail

inline Result<std::string> decompress(std::string_view input) {
  if (input.size() < 18) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "gzip input too short"));
  }
  const auto *bytes = reinterpret_cast<const unsigned char *>(input.data());
  if (bytes[0] != 0x1F || bytes[1] != 0x8B || bytes[2] != 8) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid gzip header"));
  }
  uint8_t flg = bytes[3];
  if ((flg & 0xE0u) != 0) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid gzip flags"));
  }

  size_t pos = 10;
  auto skip_cstr = [&]() -> bool {
    while (pos < input.size() && bytes[pos] != 0) ++pos;
    if (pos >= input.size()) return false;
    ++pos;
    return true;
  };

  if (flg & 0x04u) {
    if (pos + 2 > input.size()) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated gzip FEXTRA"));
    }
    uint16_t xlen = detail::read_le16(bytes + pos);
    pos += 2;
    if (xlen > input.size() - pos) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated gzip FEXTRA payload"));
    }
    pos += xlen;
  }
  if ((flg & 0x08u) && !skip_cstr()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "unterminated gzip FNAME"));
  }
  if ((flg & 0x10u) && !skip_cstr()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "unterminated gzip FCOMMENT"));
  }
  if (flg & 0x02u) {
    if (pos + 2 > input.size()) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated gzip FHCRC"));
    }
    pos += 2;
  }

  if (pos + 8 > input.size()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "missing gzip trailer"));
  }

  size_t deflate_end = input.size() - 8;
  detail::BitReader br{bytes, pos, deflate_end};
  std::string out;
  uint32_t expected_isize = detail::read_le32(bytes + deflate_end + 4);
  if (expected_isize > 0 && expected_isize <= 16u * 1024u * 1024u) {
    out.reserve(expected_isize);
  }

  while (true) {
    uint32_t bfinal = 0, btype = 0;
    if (!br.read_bits(1, bfinal) || !br.read_bits(2, btype)) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated deflate block header"));
    }
    if (btype == 0) {
      br.align_to_byte();
      uint16_t len = 0, nlen = 0;
      if (!br.read_aligned_u16(len) || !br.read_aligned_u16(nlen)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "truncated stored deflate block"));
      }
      if (len != static_cast<uint16_t>(~nlen)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "corrupt stored deflate block len"));
      }
      if (len > br.end - br.pos) {
        return std::unexpected(make_err(ErrKind::InvalidData,
                                        "truncated stored deflate payload"));
      }
      out.append(reinterpret_cast<const char *>(br.data + br.pos), len);
      br.pos += len;
    } else if (btype == 1) {
      auto res = detail::decode_fixed_block(br, out);
      if (!res) return std::unexpected(res.error());
    } else if (btype == 2) {
      auto res = detail::decode_dynamic_block(br, out);
      if (!res) return std::unexpected(res.error());
    } else {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid deflate block type"));
    }
    if (bfinal != 0) break;
  }

  br.align_to_byte();
  if (br.pos != deflate_end) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "unexpected trailing gzip data"));
  }

  uint32_t expected_crc = detail::read_le32(bytes + deflate_end);
  uint32_t actual_crc = detail::crc32_ieee(out);
  if (actual_crc != expected_crc) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "gzip CRC32 mismatch"));
  }
  uint32_t actual_isize = static_cast<uint32_t>(out.size());
  if (actual_isize != expected_isize) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "gzip ISIZE mismatch"));
  }
  return out;
}

inline std::string compress_stored(std::string_view input) {
  std::string out;
  size_t num_blocks =
      input.empty() ? 1 : ((input.size() + 65534u) / 65535u);
  out.reserve(10 + input.size() + num_blocks * 5 + 8);

  static constexpr unsigned char GZIP_HDR[10] = {
      0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
  out.append(reinterpret_cast<const char *>(GZIP_HDR), 10);

  if (input.empty()) {
    static constexpr unsigned char EMPTY_BLK[5] = {0x01, 0x00, 0x00, 0xFF, 0xFF};
    out.append(reinterpret_cast<const char *>(EMPTY_BLK), 5);
  } else {
    size_t offset = 0;
    while (offset < input.size()) {
      size_t chunk = std::min<size_t>(65535u, input.size() - offset);
      bool is_final = (offset + chunk == input.size());
      uint16_t len = static_cast<uint16_t>(chunk);
      uint16_t nlen = static_cast<uint16_t>(~len);
      unsigned char blk_hdr[5] = {
          static_cast<unsigned char>(is_final ? 0x01 : 0x00),
          static_cast<unsigned char>(len & 0xFFu),
          static_cast<unsigned char>((len >> 8) & 0xFFu),
          static_cast<unsigned char>(nlen & 0xFFu),
          static_cast<unsigned char>((nlen >> 8) & 0xFFu),
      };
      out.append(reinterpret_cast<const char *>(blk_hdr), 5);
      out.append(input.data() + offset, chunk);
      offset += chunk;
    }
  }

  uint32_t crc = detail::crc32_ieee(input);
  uint32_t isize = static_cast<uint32_t>(input.size());
  unsigned char trailer[8] = {
      static_cast<unsigned char>(crc & 0xFFu),
      static_cast<unsigned char>((crc >> 8) & 0xFFu),
      static_cast<unsigned char>((crc >> 16) & 0xFFu),
      static_cast<unsigned char>((crc >> 24) & 0xFFu),
      static_cast<unsigned char>(isize & 0xFFu),
      static_cast<unsigned char>((isize >> 8) & 0xFFu),
      static_cast<unsigned char>((isize >> 16) & 0xFFu),
      static_cast<unsigned char>((isize >> 24) & 0xFFu),
  };
  out.append(reinterpret_cast<const char *>(trailer), 8);
  return out;
}

}  // namespace gzip

// --- JSON (RFC 8259) ---

namespace json {

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
    if (const auto *p = std::get_if<std::string>(&data)) return std::string_view(*p);
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

inline const Value *Value::get(std::string_view key) const {
  const Object *obj = as_obj();
  if (!obj) return nullptr;
  auto it = obj->entries.find(std::string(key));
  if (it == obj->entries.end()) return nullptr;
  return &it->second;
}

inline Value *Value::get_mut(std::string_view key) {
  Object *obj = as_obj_mut();
  if (!obj) return nullptr;
  auto it = obj->entries.find(std::string(key));
  if (it == obj->entries.end()) return nullptr;
  return &it->second;
}

namespace detail {

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

}  // namespace detail

inline Result<Value> parse(std::string_view input) {
  detail::Parser p{input, 0};
  auto res = p.parse_value(0);
  if (!res) return std::unexpected(res.error());
  p.skip_ws();
  if (p.pos != input.size()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid JSON: trailing characters"));
  }
  return res;
}

namespace detail {

inline void append_escaped_str(std::string &out, std::string_view sv) {
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

inline void serialize_into(std::string &out, const Value &val) {
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

}  // namespace detail

inline std::string serialize(const Value &val) {
  std::string out;
  detail::serialize_into(out, val);
  return out;
}

}  // namespace json

// --- CPH-NG Integration ---

namespace cph {

struct ProblemMeta {
  fs::path bin_path;
  fs::path src_path;
  std::string name;
  std::optional<std::string> url;
  bool all_ac = false;
};

struct StageEntry {
  fs::path src_path;
  fs::path dst_path;
  std::optional<std::string> url;
  bool bin_removed = false;
};

struct StageSummary {
  std::vector<StageEntry> entries;
};

struct CleanSummary {
  std::vector<fs::path> removed_bins;
};

inline Result<ProblemMeta> read_cph_bin(const fs::path &bin_path) {
  auto raw = read_file_to_string(bin_path);
  if (!raw) return std::unexpected(raw.error());

  auto decompressed = gzip::decompress(*raw);
  if (!decompressed) {
    return std::unexpected(make_err(
        decompressed.error().kind,
        bin_path.string() + ": " + decompressed.error().message));
  }

  auto root = json::parse(*decompressed);
  if (!root) {
    return std::unexpected(make_err(
        root.error().kind, bin_path.string() + ": " + root.error().message));
  }
  if (!root->as_obj()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData,
                 bin_path.string() + ": root JSON must be an object"));
  }

  const json::Value *src_val = root->get("src");
  const json::Value *src_path_val = src_val ? src_val->get("path") : nullptr;
  auto src_path_str = src_path_val ? src_path_val->as_str() : std::nullopt;
  if (!src_path_str || src_path_str->empty()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData,
                 bin_path.string() + ": missing src.path"));
  }

  ProblemMeta meta;
  meta.bin_path = bin_path;
  meta.src_path = fs::path(*src_path_str);

  if (const json::Value *name_val = root->get("name")) {
    if (auto s = name_val->as_str()) meta.name = std::string(*s);
  }
  if (const json::Value *url_val = root->get("url")) {
    if (auto s = url_val->as_str(); s && !s->empty()) {
      meta.url = std::string(*s);
    }
  }

  if (const json::Value *tc_val = root->get("testcases")) {
    if (const json::Object *tcs = tc_val->as_obj()) {
      size_t enabled_cnt = 0;
      bool all_passed = true;
      for (const auto &[id, tc] : tcs->entries) {
        (void)id;
        if (const json::Value *dis = tc.get("isDisabled")) {
          if (auto b = dis->as_bool(); b && *b) continue;
        }
        ++enabled_cnt;
        const json::Value *res = tc.get("result");
        const json::Value *verdict = res ? res->get("verdict") : nullptr;
        auto v_str = verdict ? verdict->as_str() : std::nullopt;
        if (!v_str || *v_str != "AC") {
          all_passed = false;
        }
      }
      meta.all_ac = (enabled_cnt > 0 && all_passed);
    }
  }

  return meta;
}

namespace detail {

inline bool all_ascii_digits(std::string_view sv) {
  if (sv.empty()) return false;
  for (unsigned char c : sv) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

inline std::optional<int64_t> parse_positive_i64(std::string_view sv) {
  if (!all_ascii_digits(sv)) return std::nullopt;
  int64_t val = 0;
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
  if (ec != std::errc{} || ptr != sv.data() + sv.size() || val <= 0) {
    return std::nullopt;
  }
  return val;
}

inline std::optional<int64_t> parse_leading_positive_i64(std::string_view sv) {
  size_t len = 0;
  while (len < sv.size() &&
         std::isdigit(static_cast<unsigned char>(sv[len]))) {
    ++len;
  }
  if (len == 0) return std::nullopt;
  return parse_positive_i64(sv.substr(0, len));
}

inline std::string ascii_lower(std::string_view sv) {
  std::string out(sv);
  for (char &c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

inline bool host_matches(std::string_view host, std::string_view domain) {
  if (host == domain) return true;
  if (host.size() <= domain.size()) return false;
  return host.ends_with(domain) &&
         host[host.size() - domain.size() - 1] == '.';
}

inline std::optional<std::pair<std::string, std::string_view>> parse_http_url(
    std::string_view url) {
  auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return std::nullopt;
  std::string scheme = ascii_lower(url.substr(0, scheme_end));
  if (scheme != "http" && scheme != "https") return std::nullopt;

  std::string_view rest = url.substr(scheme_end + 3);
  size_t host_end = rest.find_first_of("/?#");
  std::string_view authority =
      (host_end == std::string_view::npos) ? rest : rest.substr(0, host_end);
  auto colon = authority.find(':');
  std::string host = ascii_lower(
      (colon == std::string_view::npos) ? authority : authority.substr(0, colon));
  if (host.empty()) return std::nullopt;

  std::string_view path;
  if (host_end != std::string_view::npos && rest[host_end] == '/') {
    std::string_view tail = rest.substr(host_end);
    size_t qh = tail.find_first_of("?#");
    path = (qh == std::string_view::npos) ? tail : tail.substr(0, qh);
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
  }
  return std::make_pair(std::move(host), path);
}

inline std::optional<int64_t> extract_yukicoder_no_from_text(
    std::string_view text, char sep) {
  for (size_t i = 0; i + 3 < text.size(); ++i) {
    if (i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1]))) {
      continue;
    }
    char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
    char c1 =
        static_cast<char>(std::tolower(static_cast<unsigned char>(text[i + 1])));
    if (c0 == 'n' && c1 == 'o' && text[i + 2] == sep) {
      size_t p = i + 3;
      while (p < text.size() && text[p] == ' ') ++p;
      if (auto no = parse_leading_positive_i64(text.substr(p))) {
        return no;
      }
    }
  }
  return std::nullopt;
}

inline void prune_empty_subdirs(const fs::path &root_dir, bool remove_root) {
  std::error_code ec;
  if (!fs::is_directory(root_dir, ec)) return;

  std::vector<fs::path> dirs;
  for (fs::recursive_directory_iterator it(root_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    auto st = it->symlink_status(ec);
    if (!ec && st.type() == fs::file_type::directory) {
      dirs.push_back(it->path());
    }
  }
  std::sort(dirs.begin(), dirs.end(), [](const fs::path &a, const fs::path &b) {
    return a.native().size() > b.native().size();
  });
  for (const auto &d : dirs) {
    if (fs::is_empty(d, ec) && !ec) {
      fs::remove(d, ec);
    }
  }
  if (remove_root && fs::is_empty(root_dir, ec) && !ec) {
    fs::remove(root_dir, ec);
  }
}

inline Result<std::map<fs::path, ProblemMeta>> load_cph_index(
    const AppConfig &config) {
  std::map<fs::path, ProblemMeta> index;
  fs::path cph_dir = config.workspace_root / ".cph-ng";
  std::error_code ec;
  if (!fs::is_directory(cph_dir, ec)) return index;

  std::vector<fs::path> bin_files;
  for (fs::recursive_directory_iterator it(cph_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    auto st = it->symlink_status(ec);
    if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
    if (st.type() == fs::file_type::regular &&
        it->path().extension() == ".bin") {
      bin_files.push_back(it->path());
    }
  }
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
  std::sort(bin_files.begin(), bin_files.end());

  for (const auto &bin_path : bin_files) {
    auto meta_res = read_cph_bin(bin_path);
    if (!meta_res) return std::unexpected(meta_res.error());
    fs::path abs_src = meta_res->src_path.is_absolute()
                           ? meta_res->src_path
                           : (config.workspace_root / meta_res->src_path);
    fs::path norm_src = normalize_path(abs_src);
    index.insert_or_assign(std::move(norm_src), std::move(*meta_res));
  }
  return index;
}

}  // namespace detail

inline std::string clean_problem_url(std::string_view url) {
  auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return std::string(url);
  std::string scheme = detail::ascii_lower(url.substr(0, scheme_end));
  if (scheme != "http" && scheme != "https") return std::string(url);

  size_t auth_start = scheme_end + 3;
  size_t auth_end = url.find_first_of("/?#", auth_start);
  if (auth_end == std::string_view::npos || url[auth_end] != '/') {
    return std::string(url);
  }
  std::string_view authority = url.substr(auth_start, auth_end - auth_start);
  auto colon = authority.find(':');
  std::string host = detail::ascii_lower(
      (colon == std::string_view::npos) ? authority : authority.substr(0, colon));
  if (!detail::host_matches(host, "qoj.ac")) return std::string(url);

  size_t path_start = auth_end;
  size_t path_end = url.find_first_of("?#", path_start);
  std::string_view path = (path_end == std::string_view::npos)
                              ? url.substr(path_start)
                              : url.substr(path_start, path_end - path_start);

  struct Seg {
    std::string_view text;
    size_t end_off;
  };
  std::vector<Seg> segs;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    if (i >= path.size()) break;
    size_t start = i;
    while (i < path.size() && path[i] != '/') ++i;
    segs.push_back(Seg{path.substr(start, i - start), i});
  }

  size_t prefix_len = 0;
  if (segs.size() >= 5 && segs[0].text == "contest" &&
      detail::all_ascii_digits(segs[1].text) && segs[2].text == "problem" &&
      detail::all_ascii_digits(segs[3].text) &&
      segs[4].text == "statement") {
    prefix_len = segs[3].end_off;
  } else if (segs.size() >= 3 && segs[0].text == "problem" &&
             detail::all_ascii_digits(segs[1].text) &&
             segs[2].text == "statement") {
    prefix_len = segs[1].end_off;
  }

  if (prefix_len > 0) {
    return std::string(url.substr(0, path_start + prefix_len));
  }
  return std::string(url);
}

inline std::optional<fs::path> route_single_problem(
    const fs::path &catalog_root, std::string_view filename,
    std::string_view name, std::string_view url) {
  if (filename.empty() || url.empty()) return std::nullopt;
  auto parsed = detail::parse_http_url(url);
  if (!parsed) return std::nullopt;
  const std::string &host = parsed->first;
  std::string_view path = parsed->second;

  if (detail::host_matches(host, "luogu.com.cn")) {
    if (!path.starts_with("/problem/")) return std::nullopt;
    std::string_view id = path.substr(9);
    if (id.size() >= 2 && id[0] == 'B' &&
        detail::all_ascii_digits(id.substr(1))) {
      return catalog_root / "luogu" / "B" / filename;
    }
    if (id.size() >= 2 && id[0] == 'P' &&
        detail::all_ascii_digits(id.substr(1))) {
      return catalog_root / "luogu" / filename;
    }
    return std::nullopt;
  }

  if (detail::host_matches(host, "judge.yosupo.jp")) {
    if (!path.starts_with("/problem/")) return std::nullopt;
    std::string_view slug = path.substr(9);
    if (slug.empty() || slug.find('/') != std::string_view::npos) {
      return std::nullopt;
    }
    return catalog_root / "library_checker" / filename;
  }

  if (detail::host_matches(host, "loj.ac")) {
    std::string_view id;
    if (path.starts_with("/p/")) id = path.substr(3);
    else if (path.starts_with("/problem/")) id = path.substr(9);
    else return std::nullopt;
    if (!detail::all_ascii_digits(id)) return std::nullopt;
    return catalog_root / "loj" / filename;
  }

  if (detail::host_matches(host, "uoj.ac")) {
    if (!path.starts_with("/problem/")) return std::nullopt;
    std::string_view id = path.substr(9);
    if (!detail::all_ascii_digits(id)) return std::nullopt;
    return catalog_root / "uoj" / filename;
  }

  if (detail::host_matches(host, "yukicoder.me")) {
    std::optional<int64_t> no;
    if (path.starts_with("/problems/no/")) {
      no = detail::parse_positive_i64(path.substr(13));
    } else if (path.starts_with("/problems/") &&
               detail::all_ascii_digits(path.substr(10))) {
      no = detail::extract_yukicoder_no_from_text(name, '.');
      if (!no) no = detail::extract_yukicoder_no_from_text(filename, '_');
    }
    if (!no || *no <= 0) return std::nullopt;

    std::string bucket;
    if (*no < 100) {
      bucket = "0001-0100";
    } else {
      int64_t lo = (*no / 100) * 100;
      int64_t hi = lo + 100;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%04lld-%04lld",
                    static_cast<long long>(lo), static_cast<long long>(hi));
      bucket = buf;
    }
    return catalog_root / "yukicoder" / bucket / filename;
  }

  return std::nullopt;
}

inline std::string prepend_url_comment_if_missing(std::string_view content,
                                                  std::string_view url) {
  if (url.empty()) return std::string(content);
  std::string_view inspect = content;
  if (inspect.starts_with("\xEF\xBB\xBF")) inspect.remove_prefix(3);
  auto nl = inspect.find('\n');
  std::string_view first_line =
      (nl == std::string_view::npos) ? inspect : inspect.substr(0, nl);
  first_line = trim_start(first_line);
  if (first_line.starts_with("//") &&
      (first_line.find("http://") != std::string_view::npos ||
       first_line.find("https://") != std::string_view::npos)) {
    return std::string(content);
  }
  std::string out;
  out.reserve(url.size() + 4 + content.size());
  out.append("// ").append(url).push_back('\n');
  out.append(content);
  return out;
}

inline Result<void> stage_file_with_url(const fs::path &src, const fs::path &dst,
                                        std::optional<std::string_view> url) {
  if (normalize_path(src) == normalize_path(dst)) {
    return std::unexpected(
        make_err(ErrKind::InvalidInput,
                 "source and destination are the same: " + src.string()));
  }

  struct stat sb{};
  if (::stat(src.c_str(), &sb) != 0) {
    return std::unexpected(from_errno(src.string()));
  }

  std::error_code ec;
  if (dst.has_parent_path()) {
    fs::create_directories(dst.parent_path(), ec);
    if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
  }

  auto content_res = read_file_to_string(src);
  if (!content_res) return std::unexpected(content_res.error());

  std::string final_content =
      (url && !url->empty())
          ? prepend_url_comment_if_missing(*content_res, *url)
          : std::move(*content_res);

  auto write_res = write_string_to_file(dst, final_content);
  if (!write_res) return write_res;

  (void)::chmod(dst.c_str(), sb.st_mode & 07777);
  struct timespec ts[2] = {sb.st_atim, sb.st_mtim};
  if (::utimensat(AT_FDCWD, dst.c_str(), ts, 0) != 0) {
    return std::unexpected(from_errno(dst.string()));
  }

  if (::unlink(src.c_str()) != 0) {
    return std::unexpected(from_errno(src.string()));
  }
  return {};
}

inline Result<StageSummary> stage_single_problems(const AppConfig &config,
                                                  bool dry_run) {
  auto idx_res = detail::load_cph_index(config);
  if (!idx_res) return std::unexpected(idx_res.error());
  const auto &index = *idx_res;

  std::error_code ec;
  fs::directory_iterator it(config.workspace_root, ec);
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));

  std::vector<fs::path> cpp_files;
  for (const auto &entry : it) {
    auto st = entry.symlink_status(ec);
    if (ec || st.type() != fs::file_type::regular) continue;
    if (entry.path().extension() == ".cpp") {
      cpp_files.push_back(entry.path());
    }
  }
  std::sort(cpp_files.begin(), cpp_files.end());

  fs::path norm_tpl = normalize_path(config.template_source);
  fs::path norm_summary = normalize_path(config.summary_path);

  StageSummary summary;
  for (const auto &src_path : cpp_files) {
    fs::path norm_src = normalize_path(src_path);
    if (norm_src == norm_tpl) continue;

    auto mit = index.find(norm_src);
    if (mit == index.end()) continue;
    const ProblemMeta &meta = mit->second;
    if (!meta.all_ac || !meta.url || meta.url->empty()) continue;

    std::string filename = src_path.filename().string();
    auto dst_opt =
        route_single_problem(config.catalog_root, filename, meta.name, *meta.url);
    if (!dst_opt) continue;

    if (normalize_path(*dst_opt) == norm_summary) {
      return std::unexpected(make_err(
          ErrKind::InvalidInput,
          "target path is reserved for summary file: " + dst_opt->string()));
    }

    std::string clean_url = clean_problem_url(*meta.url);
    bool bin_removed = false;
    if (!dry_run) {
      auto mv_res =
          stage_file_with_url(src_path, *dst_opt, std::string_view(clean_url));
      if (!mv_res) return std::unexpected(mv_res.error());
      if (::unlink(meta.bin_path.c_str()) == 0) {
        bin_removed = true;
      } else if (errno != ENOENT) {
        return std::unexpected(from_errno(meta.bin_path.string()));
      }
    } else {
      bin_removed = true;
    }

    summary.entries.push_back(StageEntry{
        .src_path = src_path,
        .dst_path = std::move(*dst_opt),
        .url = std::move(clean_url),
        .bin_removed = bin_removed,
    });
  }
  return summary;
}

inline Result<StageSummary> stage_directory(const AppConfig &config,
                                            const fs::path &src_dir_arg,
                                            const fs::path &wait_subdir_arg,
                                            bool dry_run) {
  if (src_dir_arg.empty()) {
    return std::unexpected(
        make_err(ErrKind::InvalidInput, "source directory must not be empty"));
  }
  if (wait_subdir_arg.empty() || wait_subdir_arg.is_absolute()) {
    return std::unexpected(make_err(
        ErrKind::InvalidInput,
        "wait subdir must be a relative path under catalog_root"));
  }

  fs::path src_dir = src_dir_arg.is_absolute()
                         ? src_dir_arg
                         : (config.workspace_root / src_dir_arg);
  fs::path norm_src_dir = normalize_path(src_dir);
  fs::path norm_ws = normalize_path(config.workspace_root);
  fs::path norm_cat = normalize_path(config.catalog_root);
  fs::path norm_rec = normalize_path(config.record_root);
  fs::path norm_lib = normalize_path(config.library_root);

  std::error_code ec;
  if (!fs::exists(norm_src_dir, ec) || !fs::is_directory(norm_src_dir, ec)) {
    return std::unexpected(
        make_err(ErrKind::NotFound,
                 "missing source directory: " + src_dir.string()));
  }

  if (norm_src_dir == norm_ws || norm_src_dir == norm_cat ||
      norm_src_dir == norm_rec || norm_src_dir == norm_lib ||
      path_starts_with(norm_ws, norm_src_dir) ||
      path_starts_with(norm_src_dir, norm_cat) ||
      path_starts_with(norm_src_dir, norm_rec)) {
    return std::unexpected(
        make_err(ErrKind::InvalidInput,
                 "refusing to stage protected directory: " +
                     norm_src_dir.string()));
  }

  fs::path target_base = normalize_path(config.catalog_root / wait_subdir_arg);
  if (!path_starts_with(target_base, norm_cat) || target_base == norm_cat) {
    return std::unexpected(make_err(
        ErrKind::InvalidInput,
        "wait subdir must stay strictly inside catalog_root: " +
            wait_subdir_arg.string()));
  }

  auto idx_res = detail::load_cph_index(config);
  if (!idx_res) return std::unexpected(idx_res.error());
  const auto &index = *idx_res;

  std::vector<fs::path> cpp_files;
  for (fs::recursive_directory_iterator it(norm_src_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    auto st = it->symlink_status(ec);
    if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
    if (st.type() == fs::file_type::regular &&
        it->path().extension() == ".cpp") {
      cpp_files.push_back(it->path());
    }
  }
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
  std::sort(cpp_files.begin(), cpp_files.end());

  fs::path norm_tpl = normalize_path(config.template_source);
  fs::path norm_summary = normalize_path(config.summary_path);

  StageSummary summary;
  for (const auto &src_path : cpp_files) {
    fs::path norm_file = normalize_path(src_path);
    if (norm_file == norm_tpl) continue;

    fs::path rel = norm_file.lexically_relative(norm_src_dir);
    fs::path dst = target_base / rel;
    if (normalize_path(dst) == norm_summary) {
      return std::unexpected(make_err(
          ErrKind::InvalidInput,
          "target path is reserved for summary file: " + dst.string()));
    }

    std::optional<std::string> url;
    fs::path bin_to_remove;
    auto mit = index.find(norm_file);
    if (mit != index.end()) {
      if (mit->second.url && !mit->second.url->empty()) {
        url = clean_problem_url(*mit->second.url);
      }
      bin_to_remove = mit->second.bin_path;
    }

    bool bin_removed = false;
    if (!dry_run) {
      std::optional<std::string_view> url_sv;
      if (url && !url->empty()) url_sv = *url;
      auto mv_res = stage_file_with_url(src_path, dst, url_sv);
      if (!mv_res) return std::unexpected(mv_res.error());
      if (!bin_to_remove.empty()) {
        if (::unlink(bin_to_remove.c_str()) == 0) {
          bin_removed = true;
        } else if (errno != ENOENT) {
          return std::unexpected(from_errno(bin_to_remove.string()));
        }
      }
    } else {
      bin_removed = !bin_to_remove.empty();
    }

    summary.entries.push_back(StageEntry{
        .src_path = src_path,
        .dst_path = std::move(dst),
        .url = std::move(url),
        .bin_removed = bin_removed,
    });
  }

  if (!dry_run) {
    detail::prune_empty_subdirs(norm_src_dir, true);
    detail::prune_empty_subdirs(config.workspace_root / ".cph-ng", false);
  }

  return summary;
}

inline Result<CleanSummary> clean_orphan_cph_bins(const AppConfig &config,
                                                  bool dry_run) {
  CleanSummary summary;
  fs::path cph_dir = config.workspace_root / ".cph-ng";
  std::error_code ec;
  if (!fs::exists(cph_dir, ec)) return summary;
  if (!fs::is_directory(cph_dir, ec)) return summary;

  std::vector<fs::path> bin_files;
  for (fs::recursive_directory_iterator it(cph_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    auto st = it->symlink_status(ec);
    if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
    if (st.type() == fs::file_type::regular &&
        it->path().extension() == ".bin") {
      bin_files.push_back(it->path());
    }
  }
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
  std::sort(bin_files.begin(), bin_files.end());

  fs::path norm_ws = normalize_path(config.workspace_root);
  for (const auto &bin_path : bin_files) {
    auto meta_res = read_cph_bin(bin_path);
    if (!meta_res) return std::unexpected(meta_res.error());

    fs::path raw_src = meta_res->src_path.is_absolute()
                           ? meta_res->src_path
                           : (config.workspace_root / meta_res->src_path);
    fs::path norm_src = normalize_path(raw_src);
    if (!path_starts_with(norm_src, norm_ws)) continue;

    auto src_st = fs::symlink_status(norm_src, ec);
    if (src_st.type() == fs::file_type::not_found) {
      if (!dry_run) {
        if (::unlink(bin_path.c_str()) != 0) {
          return std::unexpected(from_errno(bin_path.string()));
        }
      }
      summary.removed_bins.push_back(bin_path);
    }
  }

  if (!dry_run) {
    detail::prune_empty_subdirs(cph_dir, false);
  }
  return summary;
}

// --- Pack Contest ---

struct ContestInfo {
  std::string key;
  std::string short_id;
  std::string platform;
};

struct ContestProblemItem {
  fs::path src_path;
  ProblemMeta meta;
  std::string label;
  std::string target_filename;
  bool untouched = false;
};

struct ContestGroupSummary {
  ContestInfo info;
  std::vector<ContestProblemItem> items;
  size_t modified_count = 0;
  size_t untouched_count = 0;
};

struct PackMovedEntry {
  fs::path src_path;
  fs::path dst_path;
  fs::path old_bin;
  fs::path new_bin;
  std::string url;
};

struct PackSummary {
  ContestInfo info;
  fs::path dst_dir;
  std::vector<PackMovedEntry> packed_files;
  std::vector<fs::path> dropped_templates;
};

namespace detail {

inline std::string ascii_upper(std::string_view sv) {
  std::string out(sv);
  for (char &c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

inline bool ascii_icase_starts_with(std::string_view sv,
                                    std::string_view prefix) {
  if (sv.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(sv[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

inline std::vector<std::string_view> split_path_segments(std::string_view path) {
  std::vector<std::string_view> segs;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    if (i >= path.size()) break;
    size_t start = i;
    while (i < path.size() && path[i] != '/') ++i;
    segs.push_back(path.substr(start, i - start));
  }
  return segs;
}

inline std::optional<std::string_view> find_query_param(std::string_view url,
                                                        std::string_view key) {
  auto q = url.find('?');
  if (q == std::string_view::npos) return std::nullopt;
  std::string_view qs = url.substr(q + 1);
  auto h = qs.find('#');
  if (h != std::string_view::npos) qs = qs.substr(0, h);

  size_t pos = 0;
  while (pos <= qs.size()) {
    auto amp = qs.find('&', pos);
    std::string_view pair =
        (amp == std::string_view::npos) ? qs.substr(pos) : qs.substr(pos, amp - pos);
    auto eq = pair.find('=');
    if (eq != std::string_view::npos && pair.substr(0, eq) == key) {
      return pair.substr(eq + 1);
    }
    if (amp == std::string_view::npos) break;
    pos = amp + 1;
  }
  return std::nullopt;
}

inline std::string strip_template_prelude(std::string_view content) {
  std::string_view sv = content;
  if (sv.starts_with("\xEF\xBB\xBF")) sv.remove_prefix(3);

  auto nl = sv.find('\n');
  std::string_view first_line =
      (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
  std::string_view trimmed_first = trim_start(first_line);
  if (trimmed_first.starts_with("//") &&
      (trimmed_first.find("http://") != std::string_view::npos ||
       trimmed_first.find("https://") != std::string_view::npos)) {
    sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);
  }

  std::string clean;
  clean.reserve(sv.size());
  for (char c : sv) {
    if (c != '\r') clean.push_back(c);
  }
  return std::string(trim(clean));
}

}  // namespace detail

inline std::optional<ContestInfo> extract_contest_info(std::string_view url) {
  auto parsed = detail::parse_http_url(url);
  if (!parsed) return std::nullopt;
  const std::string &host = parsed->first;
  auto segs = detail::split_path_segments(parsed->second);

  if (detail::host_matches(host, "qoj.ac")) {
    if (segs.size() >= 4 && segs[0] == "contest" &&
        detail::all_ascii_digits(segs[1]) && segs[2] == "problem" &&
        detail::all_ascii_digits(segs[3])) {
      std::string sid(segs[1]);
      return ContestInfo{"qoj:" + sid, sid, "qoj"};
    }
    return std::nullopt;
  }

  if (detail::host_matches(host, "codeforces.com")) {
    if (segs.size() >= 4 && segs[0] == "contest" &&
        detail::all_ascii_digits(segs[1]) && segs[2] == "problem" &&
        !segs[3].empty()) {
      std::string sid(segs[1]);
      return ContestInfo{"cf:" + sid, sid, "cf"};
    }
    if (segs.size() >= 4 && segs[0] == "gym" &&
        detail::all_ascii_digits(segs[1]) && segs[2] == "problem" &&
        !segs[3].empty()) {
      std::string sid(segs[1]);
      return ContestInfo{"gym:" + sid, sid, "gym"};
    }
    return std::nullopt;
  }

  if (detail::host_matches(host, "atcoder.jp")) {
    if (segs.size() >= 4 && segs[0] == "contests" && !segs[1].empty() &&
        segs[2] == "tasks" && !segs[3].empty()) {
      std::string sid = detail::ascii_lower(segs[1]);
      return ContestInfo{"atcoder:" + sid, sid, "atcoder"};
    }
    return std::nullopt;
  }

  if (detail::host_matches(host, "uoj.ac")) {
    if (segs.size() >= 4 && segs[0] == "contest" &&
        detail::all_ascii_digits(segs[1]) && segs[2] == "problem" &&
        detail::all_ascii_digits(segs[3])) {
      std::string sid(segs[1]);
      return ContestInfo{"uoj:" + sid, sid, "uoj"};
    }
    return std::nullopt;
  }

  if (detail::host_matches(host, "ac.nowcoder.com")) {
    if (segs.size() >= 4 && segs[0] == "acm" && segs[1] == "contest" &&
        detail::all_ascii_digits(segs[2]) && !segs[3].empty()) {
      std::string sid(segs[2]);
      return ContestInfo{"nowcoder:" + sid, sid, "nowcoder"};
    }
    return std::nullopt;
  }

  if (detail::host_matches(host, "luogu.com.cn")) {
    if (auto cid = detail::find_query_param(url, "contestId")) {
      if (detail::all_ascii_digits(*cid)) {
        std::string sid(*cid);
        return ContestInfo{"luogu:" + sid, sid, "luogu"};
      }
    }
    return std::nullopt;
  }

  return std::nullopt;
}

inline std::string extract_problem_label(std::string_view name,
                                         std::string_view url,
                                         std::string_view stem) {
  std::string_view n = trim(name);

  if (detail::ascii_icase_starts_with(n, "ex")) {
    size_t j = 2;
    while (j < n.size() && std::isdigit(static_cast<unsigned char>(n[j]))) ++j;
    std::string_view rest = n.substr(j);
    if (rest.empty() || rest[0] == ' ' || rest[0] == '\t' || rest[0] == '.' ||
        rest[0] == ':' || rest[0] == '-' || rest[0] == ')' || rest[0] == ']' ||
        rest.starts_with("\xE2\x80\x93") || rest.starts_with("\xE2\x80\x94")) {
      return "Ex" + std::string(n.substr(2, j - 2));
    }
  }

  if (!n.empty() && std::isalpha(static_cast<unsigned char>(n[0]))) {
    bool all_rest_digits = true;
    for (size_t i = 1; i < n.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(n[i]))) {
        all_rest_digits = false;
        break;
      }
    }
    if (all_rest_digits) return detail::ascii_upper(n);
  }

  {
    std::string_view s = n;
    bool had_prefix = false;
    if (detail::ascii_icase_starts_with(s, "problem") && s.size() > 7 &&
        (s[7] == ' ' || s[7] == '\t')) {
      s = trim_start(s.substr(7));
      had_prefix = true;
    }
    bool bracketed = false;
    if (!s.empty() && s[0] == '[') {
      s.remove_prefix(1);
      bracketed = true;
    }
    if (!s.empty() && std::isalpha(static_cast<unsigned char>(s[0]))) {
      size_t j = 1;
      while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) {
        ++j;
      }
      std::string_view cand = s.substr(0, j);
      std::string_view rest = s.substr(j);
      if (bracketed) {
        if (!rest.empty() && rest[0] == ']') {
          return detail::ascii_upper(cand);
        }
      } else {
        std::string_view r2 = trim_start(rest);
        bool has_punct = !r2.empty() &&
                         (r2[0] == '.' || r2[0] == ':' || r2[0] == '-' ||
                          r2[0] == ')' || r2[0] == ']' ||
                          r2.starts_with("\xE2\x80\x93") ||
                          r2.starts_with("\xE2\x80\x94"));
        if (has_punct) return detail::ascii_upper(cand);
        bool upper_or_prefix = had_prefix || (s[0] >= 'A' && s[0] <= 'Z');
        if (upper_or_prefix && !rest.empty() &&
            (rest[0] == ' ' || rest[0] == '\t')) {
          return detail::ascii_upper(cand);
        }
      }
    }
  }

  if (auto parsed = detail::parse_http_url(url)) {
    const std::string &host = parsed->first;
    auto segs = detail::split_path_segments(parsed->second);
    if (detail::host_matches(host, "codeforces.com")) {
      if (segs.size() >= 4 &&
          (segs[0] == "contest" || segs[0] == "gym") &&
          segs[2] == "problem" && !segs[3].empty()) {
        return detail::ascii_upper(segs[3]);
      }
    } else if (detail::host_matches(host, "ac.nowcoder.com")) {
      if (segs.size() >= 4 && segs[0] == "acm" && segs[1] == "contest" &&
          !segs[3].empty()) {
        return detail::ascii_upper(segs[3]);
      }
    } else if (detail::host_matches(host, "atcoder.jp")) {
      if (segs.size() >= 4 && segs[0] == "contests" && segs[2] == "tasks" &&
          !segs[3].empty()) {
        auto us = segs[3].rfind('_');
        if (us != std::string_view::npos && us + 1 < segs[3].size()) {
          std::string_view tag = segs[3].substr(us + 1);
          if (detail::ascii_icase_starts_with(tag, "ex") &&
              (tag.size() == 2 || detail::all_ascii_digits(tag.substr(2)))) {
            return "Ex" + std::string(tag.substr(2));
          }
          if (tag.size() == 1 && tag[0] >= '1' && tag[0] <= '9') {
            return std::string(1, static_cast<char>('A' + (tag[0] - '1')));
          }
          return detail::ascii_upper(tag);
        }
      }
    }
  }

  return std::string(stem);
}

inline bool is_untouched_template_content(std::string_view content,
                                          std::string_view tpl_content) {
  std::string norm_content = detail::strip_template_prelude(content);
  if (norm_content.empty()) return true;
  std::string norm_tpl = detail::strip_template_prelude(tpl_content);
  return !norm_tpl.empty() && norm_content == norm_tpl;
}

inline Result<void> relocate_cph_bin(const fs::path &old_bin,
                                     const fs::path &new_bin,
                                     const fs::path &new_src_path) {
  auto raw = read_file_to_string(old_bin);
  if (!raw) return std::unexpected(raw.error());

  auto decompressed = gzip::decompress(*raw);
  if (!decompressed) {
    return std::unexpected(make_err(
        decompressed.error().kind,
        old_bin.string() + ": " + decompressed.error().message));
  }

  auto root_res = json::parse(*decompressed);
  if (!root_res) {
    return std::unexpected(make_err(
        root_res.error().kind,
        old_bin.string() + ": " + root_res.error().message));
  }

  json::Value *src_val = root_res->get_mut("src");
  json::Value *path_val = src_val ? src_val->get_mut("path") : nullptr;
  if (!path_val || !path_val->as_str()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData,
                 old_bin.string() + ": missing src.path"));
  }
  path_val->data = new_src_path.string();

  std::string out_json = json::serialize(*root_res);
  std::string out_gz = gzip::compress_stored(out_json);

  std::error_code ec;
  if (new_bin.has_parent_path()) {
    fs::create_directories(new_bin.parent_path(), ec);
    if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
  }

  auto wr = write_string_to_file(new_bin, out_gz);
  if (!wr) return wr;

  if (normalize_path(old_bin) != normalize_path(new_bin)) {
    if (::unlink(old_bin.c_str()) != 0 && errno != ENOENT) {
      return std::unexpected(from_errno(old_bin.string()));
    }
  }
  return {};
}

inline Result<std::vector<ContestGroupSummary>> list_root_contests(
    const AppConfig &config) {
  auto idx_res = detail::load_cph_index(config);
  if (!idx_res) return std::unexpected(idx_res.error());
  const auto &index = *idx_res;

  std::string tpl_content;
  std::error_code ec;
  if (fs::is_regular_file(config.template_source, ec)) {
    auto tpl_res = read_file_to_string(config.template_source);
    if (!tpl_res) return std::unexpected(tpl_res.error());
    tpl_content = std::move(*tpl_res);
  }

  fs::directory_iterator it(config.workspace_root, ec);
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));

  std::vector<fs::path> cpp_files;
  for (const auto &entry : it) {
    auto st = entry.symlink_status(ec);
    if (ec || st.type() != fs::file_type::regular) continue;
    if (entry.path().extension() == ".cpp") {
      cpp_files.push_back(entry.path());
    }
  }
  std::sort(cpp_files.begin(), cpp_files.end());

  fs::path norm_tpl = normalize_path(config.template_source);
  std::map<std::string, ContestGroupSummary> groups_map;

  for (const auto &src_path : cpp_files) {
    fs::path norm_src = normalize_path(src_path);
    if (norm_src == norm_tpl) continue;

    auto mit = index.find(norm_src);
    if (mit == index.end()) continue;
    const ProblemMeta &meta = mit->second;
    if (!meta.url || meta.url->empty()) continue;

    auto cinfo = extract_contest_info(*meta.url);
    if (!cinfo) continue;

    auto content_res = read_file_to_string(src_path);
    if (!content_res) return std::unexpected(content_res.error());

    bool untouched =
        is_untouched_template_content(*content_res, tpl_content);
    std::string stem = src_path.stem().string();
    std::string label = extract_problem_label(meta.name, *meta.url, stem);

    auto &grp = groups_map[cinfo->key];
    if (grp.info.key.empty()) grp.info = *cinfo;
    if (untouched) ++grp.untouched_count;
    else ++grp.modified_count;

    grp.items.push_back(ContestProblemItem{
        .src_path = src_path,
        .meta = meta,
        .label = std::move(label),
        .target_filename = {},
        .untouched = untouched,
    });
  }

  std::vector<ContestGroupSummary> out;
  out.reserve(groups_map.size());
  for (auto &[k, g] : groups_map) {
    (void)k;
    std::sort(g.items.begin(), g.items.end(),
              [](const ContestProblemItem &a, const ContestProblemItem &b) {
                if (a.label != b.label) return a.label < b.label;
                return a.src_path < b.src_path;
              });
    out.push_back(std::move(g));
  }
  return out;
}

inline Result<PackSummary> pack_contest(
    const AppConfig &config, std::optional<std::string_view> selector,
    const fs::path &contest_dir_arg, bool dry_run) {
  if (contest_dir_arg.empty()) {
    return std::unexpected(
        make_err(ErrKind::InvalidInput, "contest directory must not be empty"));
  }

  fs::path raw_dst = contest_dir_arg.is_absolute()
                         ? contest_dir_arg
                         : (config.workspace_root / contest_dir_arg);
  fs::path dst_dir = normalize_path(raw_dst);
  fs::path norm_ws = normalize_path(config.workspace_root);
  fs::path norm_cat = normalize_path(config.catalog_root);
  fs::path norm_rec = normalize_path(config.record_root);
  fs::path norm_lib = normalize_path(config.library_root);
  fs::path norm_cph = normalize_path(config.workspace_root / ".cph-ng");
  fs::path norm_yrs = normalize_path(config.workspace_root / "YRS");

  if (!path_starts_with(dst_dir, norm_ws) || dst_dir == norm_ws ||
      path_starts_with(dst_dir, norm_cat) ||
      path_starts_with(dst_dir, norm_rec) ||
      path_starts_with(dst_dir, norm_cph) ||
      path_starts_with(dst_dir, norm_yrs) ||
      (norm_lib != norm_ws && path_starts_with(dst_dir, norm_lib))) {
    return std::unexpected(
        make_err(ErrKind::InvalidInput,
                 "contest directory must be a non-protected subdirectory under "
                 "workspace_root: " +
                     dst_dir.string()));
  }

  std::error_code ec;
  auto dst_dir_st = fs::symlink_status(dst_dir, ec);
  if (dst_dir_st.type() != fs::file_type::not_found &&
      dst_dir_st.type() != fs::file_type::directory) {
    return std::unexpected(
        make_err(ErrKind::InvalidInput,
                 "contest path exists and is not a directory: " +
                     dst_dir.string()));
  }

  auto groups_res = list_root_contests(config);
  if (!groups_res) return std::unexpected(groups_res.error());
  auto groups = std::move(*groups_res);

  if (groups.empty()) {
    return std::unexpected(make_err(
        ErrKind::NotFound, "no contest problems found in workspace root"));
  }

  size_t selected_idx = 0;
  if (!selector || selector->empty()) {
    if (groups.size() > 1) {
      std::string keys;
      for (size_t i = 0; i < groups.size(); ++i) {
        if (i > 0) keys.append(", ");
        keys.append(groups[i].info.key);
      }
      return std::unexpected(make_err(
          ErrKind::InvalidInput,
          "multiple contests in root (" + keys + "); specify <CONTEST_ID>"));
    }
    selected_idx = 0;
  } else {
    std::string sel_lower = detail::ascii_lower(*selector);
    std::vector<size_t> matches;
    for (size_t i = 0; i < groups.size(); ++i) {
      if (detail::ascii_lower(groups[i].info.key) == sel_lower ||
          detail::ascii_lower(groups[i].info.short_id) == sel_lower) {
        matches.push_back(i);
      }
    }
    if (matches.empty()) {
      return std::unexpected(make_err(
          ErrKind::NotFound,
          "no contest matching `" + std::string(*selector) + "` in root"));
    }
    if (matches.size() > 1) {
      std::string keys;
      for (size_t i = 0; i < matches.size(); ++i) {
        if (i > 0) keys.append(", ");
        keys.append(groups[matches[i]].info.key);
      }
      return std::unexpected(make_err(
          ErrKind::InvalidInput,
          "ambiguous contest id `" + std::string(*selector) + "` matches (" +
              keys + "); specify <platform:id>"));
    }
    selected_idx = matches[0];
  }

  ContestGroupSummary &selected = groups[selected_idx];

  // Assign deduplicated filenames for modified problems
  std::set<std::string> used_names;
  for (auto &item : selected.items) {
    if (item.untouched) continue;
    std::string base =
        item.label.empty() ? item.src_path.stem().string() : item.label;
    std::string filename = base + ".cpp";
    if (!used_names.insert(filename).second) {
      std::string alt = base + "_" + item.src_path.stem().string() + ".cpp";
      size_t k = 2;
      while (!used_names.insert(alt).second) {
        alt = base + "_" + item.src_path.stem().string() + "_" +
              std::to_string(k++) + ".cpp";
      }
      filename = std::move(alt);
    }
    item.target_filename = std::move(filename);
  }

  // Conflict pre-check before touching any files
  fs::path norm_summary = normalize_path(config.summary_path);
  for (const auto &item : selected.items) {
    if (item.untouched) continue;
    fs::path dst_cpp = dst_dir / item.target_filename;
    if (normalize_path(dst_cpp) == norm_summary) {
      return std::unexpected(make_err(
          ErrKind::InvalidInput,
          "target path is reserved for summary file: " + dst_cpp.string()));
    }
    auto st = fs::symlink_status(dst_cpp, ec);
    if (st.type() != fs::file_type::not_found) {
      return std::unexpected(
          make_err(ErrKind::InvalidInput,
                   "destination file already exists: " + dst_cpp.string()));
    }
  }

  fs::path rel_dir = dst_dir.lexically_relative(norm_ws);
  PackSummary summary;
  summary.info = selected.info;
  summary.dst_dir = dst_dir;

  for (const auto &item : selected.items) {
    if (item.untouched) {
      if (!dry_run) {
        if (::unlink(item.src_path.c_str()) != 0) {
          return std::unexpected(from_errno(item.src_path.string()));
        }
        if (::unlink(item.meta.bin_path.c_str()) != 0 && errno != ENOENT) {
          return std::unexpected(from_errno(item.meta.bin_path.string()));
        }
      }
      summary.dropped_templates.push_back(item.src_path);
      continue;
    }

    fs::path dst_cpp = dst_dir / item.target_filename;
    fs::path new_bin =
        config.workspace_root / ".cph-ng" / rel_dir /
        (item.target_filename + ".bin");
    std::string clean_url = clean_problem_url(*item.meta.url);

    if (!dry_run) {
      auto mv_res =
          stage_file_with_url(item.src_path, dst_cpp, std::string_view(clean_url));
      if (!mv_res) return std::unexpected(mv_res.error());
      auto reloc_res = relocate_cph_bin(item.meta.bin_path, new_bin, dst_cpp);
      if (!reloc_res) return std::unexpected(reloc_res.error());
    }

    summary.packed_files.push_back(PackMovedEntry{
        .src_path = item.src_path,
        .dst_path = std::move(dst_cpp),
        .old_bin = item.meta.bin_path,
        .new_bin = std::move(new_bin),
        .url = std::move(clean_url),
    });
  }

  return summary;
}

}  // namespace cph

}  // namespace yrs

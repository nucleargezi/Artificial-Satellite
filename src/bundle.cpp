#include "bundle.hpp"

#include "core/str_util.hpp"

#include <cstdint>
#include <iterator>
#include <set>
#include <string_view>
#include <sys/stat.h>

namespace yrs {

namespace {

struct MtimeCandidate {
  int64_t sec;
  int64_t nsec;
  fs::path norm_path;
  fs::path raw_path;

  bool is_older_than(const MtimeCandidate &other) const {
    if (sec != other.sec) return sec < other.sec;
    if (nsec != other.nsec) return nsec < other.nsec;
    return norm_path < other.norm_path;
  }
};

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

std::string strip_line_comment(std::string_view line) {
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

std::optional<std::string_view> parse_local_include(std::string_view line) {
  if (!line.starts_with("#include")) return std::nullopt;
  std::string_view rest = trim_start(line.substr(8));
  if (rest.empty() || rest.front() != '"') return std::nullopt;
  rest.remove_prefix(1);
  auto end = rest.find('"');
  if (end == std::string_view::npos) return std::nullopt;
  if (!trim(rest.substr(end + 1)).empty()) return std::nullopt;
  return rest.substr(0, end);
}

std::optional<fs::path> resolve_include_path(const fs::path &current_file,
                                             const fs::path &library_root,
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

Result<std::vector<std::string>> expand_file(const fs::path &path,
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

std::string normalize_lines(std::vector<std::string> lines) {
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

}  // namespace

Result<std::optional<fs::path>> find_latest_active_cpp(
    const AppConfig &config, const fs::path &cwd) {
  fs::path norm_ws = normalize_path(config.workspace_root);
  fs::path search_root = config.workspace_root;

  if (!cwd.empty()) {
    fs::path norm_cwd = normalize_path(cwd);
    if (path_starts_with(norm_cwd, norm_ws) && norm_cwd != norm_ws) {
      if (is_protected_workspace_dir(config, norm_cwd)) {
        return std::unexpected(make_err(
            ErrKind::InvalidInput,
            "refusing to operate inside protected directory: " +
                norm_cwd.string()));
      }
      search_root = cwd;
    }
  }

  auto files_res = collect_active_cpps_recursive(config, search_root);
  if (!files_res) return std::unexpected(files_res.error());

  std::optional<MtimeCandidate> best;
  for (const auto &p : *files_res) {
    struct stat sb{};
    if (::stat(p.c_str(), &sb) != 0) {
      return std::unexpected(from_errno(p.string()));
    }
    MtimeCandidate cand{
        .sec = sb.st_mtim.tv_sec,
        .nsec = sb.st_mtim.tv_nsec,
        .norm_path = normalize_path(p),
        .raw_path = p,
    };
    if (!best || best->is_older_than(cand)) {
      best = std::move(cand);
    }
  }
  if (!best) return std::nullopt;
  return best->raw_path;
}

Result<fs::path> cover_latest_cpp(const AppConfig &config, const fs::path &cwd,
                                  bool dry_run) {
  std::error_code ec;
  if (!fs::is_regular_file(config.template_source, ec)) {
    return std::unexpected(make_err(
        ErrKind::NotFound,
        "no default file: " + config.template_source.string()));
  }

  auto tpl_content = read_file_to_string(config.template_source);
  if (!tpl_content) return std::unexpected(tpl_content.error());

  auto latest_res = find_latest_active_cpp(config, cwd);
  if (!latest_res) return std::unexpected(latest_res.error());
  if (!latest_res->has_value()) {
    fs::path scope = cwd.empty() ? config.workspace_root : cwd;
    return std::unexpected(
        make_err(ErrKind::NotFound, "no cpp file in " + scope.string()));
  }

  const fs::path &target = **latest_res;
  if (!dry_run) {
    auto write_res = write_string_to_file(target, *tpl_content);
    if (!write_res) return std::unexpected(write_res.error());
  }

  return target;
}

Result<BundledLatest> bundle_cpp_path(const AppConfig &config,
                                      const fs::path &source_path) {
  fs::path library_root = normalize_path(config.library_root);
  fs::path norm_source = normalize_path(source_path);
  VisitedFiles visited;
  auto lines_res = expand_file(norm_source, library_root, visited);
  if (!lines_res) return std::unexpected(lines_res.error());

  return BundledLatest{
      .source_path = norm_source,
      .content = normalize_lines(std::move(*lines_res)),
      .visited_local_files = std::move(visited.ordered),
  };
}

Result<BundledLatest> bundle_latest_cpp(const AppConfig &config,
                                        const fs::path &cwd) {
  auto latest_res = find_latest_active_cpp(config, cwd);
  if (!latest_res) return std::unexpected(latest_res.error());
  if (!latest_res->has_value()) {
    fs::path scope = cwd.empty() ? config.workspace_root : cwd;
    return std::unexpected(
        make_err(ErrKind::NotFound, "no cpp file in " + scope.string()));
  }
  return bundle_cpp_path(config, **latest_res);
}

}  // namespace yrs

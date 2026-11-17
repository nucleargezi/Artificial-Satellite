#include "config.hpp"

#include "core/str_util.hpp"

#include <algorithm>
#include <map>

namespace yrs {

AppConfig AppConfig::defaults_for_dir(const fs::path &dir) {
  return AppConfig{
      .workspace_root = dir,
      .library_root = dir,
      .catalog_root = dir / ".wait",
      .record_root = dir / "record",
      .summary_path = dir / "record" / "summary.md",
      .template_source = dir / "0.cpp",
  };
}

Result<fs::path> AppConfig::restore_summary_path(const fs::path &record_root,
                                                 const fs::path &summary_file) {
  auto fail = [&]() -> Result<fs::path> {
    return std::unexpected(make_err(
        ErrKind::InvalidData,
        "summary_file must be a filename under " + record_root.string()));
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

Result<std::string> AppConfig::parse_toml_string(std::string_view raw,
                                                 const fs::path &config_path) {
  raw = trim(raw);
  if (raw.size() < 2 || raw.front() != '"') {
    return std::unexpected(
        make_err(ErrKind::InvalidData,
                 "invalid " + config_path.string() + ": expected quoted string"));
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
          return std::unexpected(make_err(
              ErrKind::InvalidData,
              "invalid " + config_path.string() + ": unsupported escape sequence"));
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
        return std::unexpected(make_err(
            ErrKind::InvalidData,
            "invalid " + config_path.string() + ": trailing characters after string"));
      }
      return out;
    }
    out.push_back(c);
  }
  return std::unexpected(
      make_err(ErrKind::InvalidData,
               "invalid " + config_path.string() + ": unterminated string"));
}

Result<AppConfig> AppConfig::load_for_dir(const fs::path &dir) {
  AppConfig config = defaults_for_dir(dir);
  fs::path config_path = dir / "yrs.toml";
  std::error_code ec;
  bool exists = fs::exists(config_path, ec);
  if (ec) {
    return std::unexpected(make_err(ErrKind::Other, ec.message()));
  }
  if (!exists) {
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
                   "invalid " + config_path.string() + ": expected key = value"));
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

Result<fs::path> AppConfig::find_workspace_root(const fs::path &start_dir) {
  if (start_dir.empty()) {
    return std::unexpected(
        make_err(ErrKind::InvalidInput, "empty start directory"));
  }
  std::error_code ec;
  fs::path cur = normalize_path(start_dir);
  while (true) {
    fs::path cand = cur / "yrs.toml";
    if (fs::exists(cand, ec) && !fs::is_directory(cand, ec)) {
      return cur;
    }
    ec.clear();
    fs::path parent = cur.parent_path();
    if (parent.empty() || parent == cur) break;
    cur = std::move(parent);
  }
  return std::unexpected(make_err(
      ErrKind::NotFound,
      "missing yrs.toml in " + start_dir.string() + " or any parent directory"));
}

Result<AppConfig> AppConfig::discover_for_dir(const fs::path &start_dir) {
  auto root_res = find_workspace_root(start_dir);
  if (!root_res) return std::unexpected(root_res.error());
  return load_for_dir(*root_res);
}

bool is_protected_workspace_dir(const AppConfig &config, const fs::path &dir) {
  fs::path norm_dir = normalize_path(dir);
  fs::path norm_ws = normalize_path(config.workspace_root);
  if (norm_dir == norm_ws) return false;

  fs::path norm_cat = normalize_path(config.catalog_root);
  fs::path norm_rec = normalize_path(config.record_root);
  fs::path norm_lib = normalize_path(config.library_root);
  fs::path norm_cph = normalize_path(config.workspace_root / ".cph-ng");
  fs::path norm_yrs = normalize_path(config.workspace_root / "YRS");

  if (path_starts_with(norm_dir, norm_cat) ||
      path_starts_with(norm_dir, norm_rec) ||
      path_starts_with(norm_dir, norm_cph) ||
      path_starts_with(norm_dir, norm_yrs) ||
      (norm_lib != norm_ws && path_starts_with(norm_dir, norm_lib))) {
    return true;
  }

  if (path_starts_with(norm_dir, norm_ws)) {
    fs::path rel = norm_dir.lexically_relative(norm_ws);
    for (const auto &comp : rel) {
      std::string s = comp.string();
      if (!s.empty() && s != "." && s != ".." && s.front() == '.') {
        return true;
      }
    }
  }
  return false;
}

Result<WorkspaceRootScan> scan_workspace_root(const AppConfig &config) {
  std::error_code ec;
  fs::directory_iterator it(config.workspace_root, ec);
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));

  fs::path norm_tpl = normalize_path(config.template_source);
  WorkspaceRootScan scan;

  for (const auto &entry : it) {
    auto st = entry.symlink_status(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    const fs::path &p = entry.path();
    if (st.type() == fs::file_type::directory) {
      if (!is_protected_workspace_dir(config, p)) {
        scan.active_subdirs.push_back(p);
      }
    } else if (st.type() == fs::file_type::regular &&
               p.extension() == ".cpp") {
      if (normalize_path(p) != norm_tpl) {
        scan.root_cpps.push_back(p);
      }
    }
  }

  std::sort(scan.root_cpps.begin(), scan.root_cpps.end());
  std::sort(scan.active_subdirs.begin(), scan.active_subdirs.end());
  return scan;
}

namespace {

Result<void> collect_active_rec(const fs::path &cur_dir,
                                const AppConfig &config,
                                const fs::path &norm_tpl,
                                std::vector<fs::path> &out) {
  std::error_code ec;
  fs::directory_iterator it(cur_dir, ec);
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));

  for (const auto &entry : it) {
    auto st = entry.symlink_status(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    const fs::path &p = entry.path();
    if (st.type() == fs::file_type::directory) {
      if (!is_protected_workspace_dir(config, p)) {
        auto sub = collect_active_rec(p, config, norm_tpl, out);
        if (!sub) return sub;
      }
      continue;
    }
    if (st.type() == fs::file_type::regular && p.extension() == ".cpp") {
      if (normalize_path(p) != norm_tpl) {
        out.push_back(p);
      }
    }
  }
  return {};
}

}  // namespace

Result<std::vector<fs::path>> collect_active_cpps_recursive(
    const AppConfig &config, const fs::path &root_dir) {
  std::vector<fs::path> out;
  fs::path norm_tpl = normalize_path(config.template_source);
  auto res = collect_active_rec(root_dir, config, norm_tpl, out);
  if (!res) return std::unexpected(res.error());
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace yrs

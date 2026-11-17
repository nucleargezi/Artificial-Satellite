#include "cph/workflow.hpp"

#include "bundle.hpp"
#include "core/str_util.hpp"
#include "ui/term.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace yrs::cph {

namespace {

Result<std::string> read_optional_template(const AppConfig &config) {
  std::error_code ec;
  if (!fs::is_regular_file(config.template_source, ec)) {
    return std::string{};
  }
  return read_file_to_string(config.template_source);
}

Result<bool> unlink_bins(const std::vector<fs::path> &bins,
                         std::optional<fs::path> keep_norm = std::nullopt) {
  bool removed_any = false;
  for (const auto &b : bins) {
    if (keep_norm && normalize_path(b) == *keep_norm) continue;
    if (::unlink(b.c_str()) == 0) {
      removed_any = true;
    } else if (errno != ENOENT) {
      return std::unexpected(from_errno(b.string()));
    }
  }
  return removed_any;
}

StatusProblemItem build_status_item_from_meta(const fs::path &src_path,
                                              const ProblemMeta &meta,
                                              bool untouched) {
  StatusProblemItem item;
  item.src_path = src_path;
  item.enabled_tc = meta.enabled_tc;
  item.ac_tc = meta.ac_tc;

  if (untouched) {
    item.state = ProblemStateKind::Untouched;
    item.verdict_tag = "untouched";
    return item;
  }

  std::string ratio =
      std::to_string(meta.ac_tc) + "/" + std::to_string(meta.enabled_tc);
  if (meta.enabled_tc == 0) {
    item.state = ProblemStateKind::NoTests;
    item.verdict_tag = "no tests";
  } else if (meta.all_ac) {
    item.state = ProblemStateKind::Ac;
    item.verdict_tag = "AC " + ratio;
  } else if (meta.failed_tc > 0) {
    item.state = ProblemStateKind::Failed;
    std::string v =
        meta.first_fail_verdict.empty() ? "FAIL" : meta.first_fail_verdict;
    item.verdict_tag = v + " " + ratio;
  } else if (meta.ac_tc > 0) {
    item.state = ProblemStateKind::Partial;
    item.verdict_tag = "partial " + ratio;
  } else {
    item.state = ProblemStateKind::Unrun;
    item.verdict_tag = "unrun " + ratio;
  }
  return item;
}

Result<void> collect_catalog_files_rec(const fs::path &cur_dir,
                                       const fs::path &norm_cat,
                                       const fs::path &norm_summary,
                                       size_t &total_files,
                                       std::map<std::string, size_t> &buckets) {
  std::error_code ec;
  fs::directory_iterator it(cur_dir, ec);
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));

  for (const auto &entry : it) {
    auto st = entry.symlink_status(ec);
    if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
    const fs::path &p = entry.path();
    if (st.type() == fs::file_type::directory) {
      auto sub = collect_catalog_files_rec(p, norm_cat, norm_summary,
                                           total_files, buckets);
      if (!sub) return sub;
      continue;
    }
    if (st.type() != fs::file_type::regular) continue;
    fs::path norm_p = normalize_path(p);
    if (norm_p == norm_summary) continue;

    ++total_files;
    fs::path rel = norm_p.lexically_relative(norm_cat);
    std::vector<std::string> parts;
    for (const auto &c : rel) {
      if (!c.empty() && c != ".") parts.push_back(c.string());
    }
    std::string bucket = (parts.size() >= 2) ? parts[0] : "(root)";
    ++buckets[bucket];
  }
  return {};
}

}  // namespace

Result<void> stage_file_with_url(const fs::path &src, const fs::path &dst,
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

Result<StageSummary> stage_single_problems(const AppConfig &config,
                                           bool dry_run) {
  auto idx_res = scan_cph_index(config);
  if (!idx_res) return std::unexpected(idx_res.error());
  const auto &index = idx_res->by_source;

  auto scan_res = scan_workspace_root(config);
  if (!scan_res) return std::unexpected(scan_res.error());

  fs::path norm_summary = normalize_path(config.summary_path);
  StageSummary summary;

  for (const auto &src_path : scan_res->root_cpps) {
    fs::path norm_src = normalize_path(src_path);
    auto mit = index.find(norm_src);
    if (mit == index.end()) continue;
    const IndexedProblem &ip = mit->second;
    const ProblemMeta &meta = ip.meta;
    if (!meta.all_ac || !meta.url || meta.url->empty()) continue;

    std::string filename = src_path.filename().string();
    auto dst_opt = route_single_problem(config.catalog_root, filename,
                                        meta.name, *meta.url);
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
      auto rm_res = unlink_bins(ip.bin_paths);
      if (!rm_res) return std::unexpected(rm_res.error());
      bin_removed = *rm_res;
    } else {
      bin_removed = !ip.bin_paths.empty();
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

Result<StageSummary> stage_directory(const AppConfig &config,
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
    return std::unexpected(make_err(
        ErrKind::NotFound, "missing source directory: " + src_dir.string()));
  }

  if (norm_src_dir == norm_ws || norm_src_dir == norm_cat ||
      norm_src_dir == norm_rec || norm_lib == norm_src_dir ||
      path_starts_with(norm_ws, norm_src_dir) ||
      is_protected_workspace_dir(config, norm_src_dir)) {
    return std::unexpected(make_err(
        ErrKind::InvalidInput,
        "refusing to stage protected directory: " + norm_src_dir.string()));
  }

  fs::path target_base = normalize_path(config.catalog_root / wait_subdir_arg);
  if (!path_starts_with(target_base, norm_cat) || target_base == norm_cat) {
    return std::unexpected(make_err(
        ErrKind::InvalidInput,
        "wait subdir must stay strictly inside catalog_root: " +
            wait_subdir_arg.string()));
  }

  auto idx_res = scan_cph_index(config);
  if (!idx_res) return std::unexpected(idx_res.error());
  const auto &index = idx_res->by_source;

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
    const std::vector<fs::path> *bins_ptr = nullptr;
    auto mit = index.find(norm_file);
    if (mit != index.end()) {
      const IndexedProblem &ip = mit->second;
      if (ip.meta.url && !ip.meta.url->empty()) {
        url = clean_problem_url(*ip.meta.url);
      }
      bins_ptr = &ip.bin_paths;
    }

    bool bin_removed = false;
    if (!dry_run) {
      std::optional<std::string_view> url_sv;
      if (url && !url->empty()) url_sv = *url;
      auto mv_res = stage_file_with_url(src_path, dst, url_sv);
      if (!mv_res) return std::unexpected(mv_res.error());
      if (bins_ptr != nullptr && !bins_ptr->empty()) {
        auto rm_res = unlink_bins(*bins_ptr);
        if (!rm_res) return std::unexpected(rm_res.error());
        bin_removed = *rm_res;
      }
    } else {
      bin_removed = (bins_ptr != nullptr && !bins_ptr->empty());
    }

    summary.entries.push_back(StageEntry{
        .src_path = src_path,
        .dst_path = std::move(dst),
        .url = std::move(url),
        .bin_removed = bin_removed,
    });
  }

  if (!dry_run) {
    prune_empty_subdirs(norm_src_dir, true);
    prune_empty_subdirs(config.workspace_root / ".cph-ng", false);
  }

  return summary;
}

Result<CleanSummary> clean_cph_workspace(const AppConfig &config, bool dry_run,
                                         bool include_unused) {
  CleanSummary summary;
  fs::path cph_dir = config.workspace_root / ".cph-ng";
  std::error_code ec;
  if (!fs::is_directory(cph_dir, ec)) return summary;

  auto idx_res = scan_cph_index(config);
  if (!idx_res) return std::unexpected(idx_res.error());
  auto &idx = *idx_res;

  for (const auto &bin_path : idx.orphan_bins) {
    if (!dry_run) {
      if (::unlink(bin_path.c_str()) != 0 && errno != ENOENT) {
        return std::unexpected(from_errno(bin_path.string()));
      }
    }
    summary.removed_bins.push_back(bin_path);
  }

  if (include_unused) {
    auto tpl_res = read_optional_template(config);
    if (!tpl_res) return std::unexpected(tpl_res.error());
    const std::string &tpl_content = *tpl_res;

    fs::path norm_ws = normalize_path(config.workspace_root);
    fs::path norm_tpl = normalize_path(config.template_source);
    std::set<fs::path> touched_top_subdirs;

    for (auto &[norm_src, ip] : idx.by_source) {
      if (!path_starts_with(norm_src, norm_ws)) continue;
      auto src_st = fs::symlink_status(norm_src, ec);
      if (ec || src_st.type() != fs::file_type::regular) {
        ec.clear();
        continue;
      }
      if (norm_src.extension() != ".cpp") continue;
      if (norm_src == norm_tpl) continue;
      fs::path parent_dir = norm_src.parent_path();
      if (is_protected_workspace_dir(config, parent_dir)) continue;

      auto content_res = read_file_to_string(norm_src);
      if (!content_res) return std::unexpected(content_res.error());
      if (!is_untouched_template_content(*content_res, tpl_content)) continue;

      if (!dry_run) {
        if (::unlink(norm_src.c_str()) != 0 && errno != ENOENT) {
          return std::unexpected(from_errno(norm_src.string()));
        }
        auto rm_res = unlink_bins(ip.bin_paths);
        if (!rm_res) return std::unexpected(rm_res.error());

        if (parent_dir != norm_ws && path_starts_with(parent_dir, norm_ws)) {
          fs::path rel_parent = parent_dir.lexically_relative(norm_ws);
          auto it_comp = rel_parent.begin();
          if (it_comp != rel_parent.end() && !it_comp->empty() &&
              *it_comp != ".") {
            touched_top_subdirs.insert(norm_ws / *it_comp);
          }
        }
      }
      summary.removed_unused.push_back(
          UnusedProblemEntry{norm_src, std::move(ip.bin_paths)});
    }

    if (!dry_run) {
      for (const auto &sub : touched_top_subdirs) {
        prune_empty_subdirs(sub, true);
      }
    }
  }

  if (!dry_run) {
    prune_empty_subdirs(cph_dir, false);
  }
  return summary;
}

Result<CleanSummary> clean_orphan_cph_bins(const AppConfig &config,
                                           bool dry_run) {
  return clean_cph_workspace(config, dry_run, false);
}

Result<std::vector<ContestGroupSummary>> list_root_contests(
    const AppConfig &config) {
  auto idx_res = scan_cph_index(config);
  if (!idx_res) return std::unexpected(idx_res.error());
  const auto &index = idx_res->by_source;

  auto tpl_res = read_optional_template(config);
  if (!tpl_res) return std::unexpected(tpl_res.error());
  const std::string &tpl_content = *tpl_res;

  auto scan_res = scan_workspace_root(config);
  if (!scan_res) return std::unexpected(scan_res.error());

  std::map<std::string, ContestGroupSummary> groups_map;

  for (const auto &src_path : scan_res->root_cpps) {
    fs::path norm_src = normalize_path(src_path);
    auto mit = index.find(norm_src);
    if (mit == index.end()) continue;
    const IndexedProblem &ip = mit->second;
    const ProblemMeta &meta = ip.meta;
    if (!meta.url || meta.url->empty()) continue;

    auto cinfo = extract_contest_info(*meta.url);
    if (!cinfo) continue;

    auto content_res = read_file_to_string(src_path);
    if (!content_res) return std::unexpected(content_res.error());

    bool untouched = is_untouched_template_content(*content_res, tpl_content);
    std::string stem = src_path.stem().string();
    std::string label = extract_problem_label(meta.name, *meta.url, stem);

    auto &grp = groups_map[cinfo->key];
    if (grp.info.key.empty()) grp.info = *cinfo;
    if (untouched) ++grp.untouched_count;
    else ++grp.modified_count;

    grp.items.push_back(ContestProblemItem{
        .src_path = src_path,
        .meta = meta,
        .bin_paths = ip.bin_paths,
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
                if (a.label != b.label) {
                  return natural_less(a.label, b.label);
                }
                return a.src_path < b.src_path;
              });
    out.push_back(std::move(g));
  }
  std::sort(out.begin(), out.end(),
            [](const ContestGroupSummary &a, const ContestGroupSummary &b) {
              return natural_less(a.info.key, b.info.key);
            });
  return out;
}

Result<PackSummary> pack_contest(const AppConfig &config,
                                 std::optional<std::string_view> selector,
                                 const fs::path &contest_dir_arg,
                                 bool dry_run) {
  if (contest_dir_arg.empty()) {
    return std::unexpected(
        make_err(ErrKind::InvalidInput, "contest directory must not be empty"));
  }

  fs::path raw_dst = contest_dir_arg.is_absolute()
                         ? contest_dir_arg
                         : (config.workspace_root / contest_dir_arg);
  fs::path dst_dir = normalize_path(raw_dst);
  fs::path norm_ws = normalize_path(config.workspace_root);

  if (!path_starts_with(dst_dir, norm_ws) || dst_dir == norm_ws ||
      is_protected_workspace_dir(config, dst_dir)) {
    return std::unexpected(make_err(
        ErrKind::InvalidInput,
        "contest directory must be a non-protected subdirectory under "
        "workspace_root: " +
            dst_dir.string()));
  }

  std::error_code ec;
  auto dst_dir_st = fs::symlink_status(dst_dir, ec);
  if (dst_dir_st.type() != fs::file_type::not_found &&
      dst_dir_st.type() != fs::file_type::directory) {
    return std::unexpected(make_err(
        ErrKind::InvalidInput,
        "contest path exists and is not a directory: " + dst_dir.string()));
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
    std::string sel_lower = ascii_lower(*selector);
    std::vector<size_t> matches;
    for (size_t i = 0; i < groups.size(); ++i) {
      if (ascii_lower(groups[i].info.key) == sel_lower ||
          ascii_lower(groups[i].info.short_id) == sel_lower) {
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
      return std::unexpected(make_err(
          ErrKind::InvalidInput,
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
        auto rm_res = unlink_bins(item.bin_paths);
        if (!rm_res) return std::unexpected(rm_res.error());
      }
      summary.dropped_templates.push_back(item.src_path);
      continue;
    }

    fs::path dst_cpp = dst_dir / item.target_filename;
    fs::path new_bin = config.workspace_root / ".cph-ng" / rel_dir /
                       (item.target_filename + ".bin");
    std::string clean_url = clean_problem_url(*item.meta.url);

    if (!dry_run) {
      auto mv_res =
          stage_file_with_url(item.src_path, dst_cpp, std::string_view(clean_url));
      if (!mv_res) return std::unexpected(mv_res.error());
      auto reloc_res = relocate_cph_bin(item.meta.bin_path, new_bin, dst_cpp);
      if (!reloc_res) return std::unexpected(reloc_res.error());
      auto extra_rm = unlink_bins(item.bin_paths, normalize_path(new_bin));
      if (!extra_rm) return std::unexpected(extra_rm.error());
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

Result<WorkspaceStatus> inspect_workspace_status(const AppConfig &config,
                                                 const fs::path &cwd) {
  WorkspaceStatus status;
  status.workspace_root = config.workspace_root;
  status.cwd = cwd.empty() ? config.workspace_root : cwd;

  auto latest_res = find_latest_active_cpp(config, status.cwd);
  if (latest_res) {
    status.latest_active_cpp = *latest_res;
  } else if (latest_res.error().kind != ErrKind::InvalidInput) {
    return std::unexpected(latest_res.error());
  }

  auto idx_res = scan_cph_index(config);
  if (!idx_res) return std::unexpected(idx_res.error());
  const auto &index = idx_res->by_source;
  status.orphan_bin_count = idx_res->orphan_bins.size();

  auto tpl_res = read_optional_template(config);
  if (!tpl_res) return std::unexpected(tpl_res.error());
  const std::string &tpl_content = *tpl_res;

  auto scan_res = scan_workspace_root(config);
  if (!scan_res) return std::unexpected(scan_res.error());

  fs::path norm_ws = normalize_path(config.workspace_root);
  fs::path norm_summary = normalize_path(config.summary_path);

  std::map<std::string, StatusContestGroup> contest_map;
  for (const auto &src_path : scan_res->root_cpps) {
    fs::path norm_src = normalize_path(src_path);
    auto mit = index.find(norm_src);
    if (mit == index.end()) {
      status.local_root_files.push_back(src_path);
      continue;
    }

    const ProblemMeta &meta = mit->second.meta;
    auto content_res = read_file_to_string(src_path);
    if (!content_res) return std::unexpected(content_res.error());
    bool untouched = is_untouched_template_content(*content_res, tpl_content);
    if (untouched) ++status.total_unused_cph_count;

    std::optional<ContestInfo> cinfo;
    if (meta.url && !meta.url->empty()) {
      cinfo = extract_contest_info(*meta.url);
    }

    if (cinfo) {
      StatusProblemItem item =
          build_status_item_from_meta(src_path, meta, untouched);
      item.label =
          extract_problem_label(meta.name, *meta.url, src_path.stem().string());
      auto &grp = contest_map[cinfo->key];
      if (grp.info.key.empty()) grp.info = *cinfo;
      if (untouched) ++grp.untouched_count;
      else if (meta.all_ac) ++grp.ac_count;
      else ++grp.wip_count;
      grp.items.push_back(std::move(item));
      continue;
    }

    std::optional<fs::path> stage_dst;
    if (meta.all_ac && meta.url && !meta.url->empty()) {
      stage_dst = route_single_problem(
          config.catalog_root, src_path.filename().string(), meta.name,
          *meta.url);
    }

    if (stage_dst) {
      StatusProblemItem item =
          build_status_item_from_meta(src_path, meta, false);
      item.stage_dst = std::move(stage_dst);
      status.ready_to_stage.push_back(std::move(item));
    } else if (untouched) {
      StatusProblemItem item =
          build_status_item_from_meta(src_path, meta, true);
      status.untouched_singles.push_back(std::move(item));
    } else {
      StatusProblemItem item =
          build_status_item_from_meta(src_path, meta, false);
      if (meta.all_ac) {
        item.unrouted_ac = true;
        item.verdict_tag.append(", manual");
      }
      status.in_progress_singles.push_back(std::move(item));
    }
  }

  auto by_src_name = [](const StatusProblemItem &a,
                        const StatusProblemItem &b) {
    std::string na = a.src_path.filename().string();
    std::string nb = b.src_path.filename().string();
    if (na != nb) return natural_less(na, nb);
    return a.src_path < b.src_path;
  };
  std::sort(status.ready_to_stage.begin(), status.ready_to_stage.end(),
            by_src_name);
  std::sort(status.in_progress_singles.begin(),
            status.in_progress_singles.end(), by_src_name);
  std::sort(status.untouched_singles.begin(), status.untouched_singles.end(),
            by_src_name);
  std::sort(status.local_root_files.begin(), status.local_root_files.end(),
            [](const fs::path &a, const fs::path &b) {
              std::string na = a.filename().string();
              std::string nb = b.filename().string();
              if (na != nb) return natural_less(na, nb);
              return a < b;
            });

  status.root_contests.reserve(contest_map.size());
  for (auto &[k, grp] : contest_map) {
    (void)k;
    std::sort(grp.items.begin(), grp.items.end(),
              [](const StatusProblemItem &a, const StatusProblemItem &b) {
                if (a.label != b.label) {
                  return natural_less(a.label, b.label);
                }
                return a.src_path < b.src_path;
              });
    status.root_contests.push_back(std::move(grp));
  }
  std::sort(status.root_contests.begin(), status.root_contests.end(),
            [](const StatusContestGroup &a, const StatusContestGroup &b) {
              return natural_less(a.info.key, b.info.key);
            });

  for (const auto &sub_dir : scan_res->active_subdirs) {
    auto col_res = collect_active_cpps_recursive(config, sub_dir);
    if (!col_res) return std::unexpected(col_res.error());
    if (col_res->empty()) continue;

    fs::path norm_sub = normalize_path(sub_dir);
    StatusSubdirGroup grp;
    grp.dir_path = sub_dir;
    grp.rel_name = rel_display(norm_ws, sub_dir);

    for (const auto &p : *col_res) {
      fs::path norm_p = normalize_path(p);
      std::string rel_in_sub = rel_display(norm_sub, p);
      auto mit = index.find(norm_p);
      if (mit == index.end()) {
        StatusProblemItem item;
        item.src_path = p;
        item.label = std::move(rel_in_sub);
        item.state = ProblemStateKind::LocalNoCph;
        item.verdict_tag = "local";
        ++grp.local_count;
        grp.items.push_back(std::move(item));
        continue;
      }

      const ProblemMeta &meta = mit->second.meta;
      auto content_res = read_file_to_string(p);
      if (!content_res) return std::unexpected(content_res.error());
      bool untouched = is_untouched_template_content(*content_res, tpl_content);
      if (untouched) {
        ++status.total_unused_cph_count;
        ++grp.untouched_count;
      } else if (meta.all_ac) {
        ++grp.ac_count;
      } else {
        ++grp.wip_count;
      }

      StatusProblemItem item = build_status_item_from_meta(p, meta, untouched);
      item.label = std::move(rel_in_sub);
      grp.items.push_back(std::move(item));
    }

    std::sort(grp.items.begin(), grp.items.end(),
              [](const StatusProblemItem &a, const StatusProblemItem &b) {
                if (a.label != b.label) {
                  return natural_less(a.label, b.label);
                }
                return a.src_path < b.src_path;
              });
    status.subdir_workspaces.push_back(std::move(grp));
  }
  std::sort(status.subdir_workspaces.begin(), status.subdir_workspaces.end(),
            [](const StatusSubdirGroup &a, const StatusSubdirGroup &b) {
              return natural_less(a.rel_name, b.rel_name);
            });

  std::error_code ec;
  if (fs::is_directory(config.catalog_root, ec)) {
    fs::path norm_cat = normalize_path(config.catalog_root);
    std::map<std::string, size_t> buckets;
    auto cat_res = collect_catalog_files_rec(config.catalog_root, norm_cat,
                                             norm_summary,
                                             status.catalog_file_count, buckets);
    if (!cat_res) return std::unexpected(cat_res.error());
    status.catalog_buckets.reserve(buckets.size());
    for (auto &[name, cnt] : buckets) {
      status.catalog_buckets.push_back(CatalogBucketCount{name, cnt});
    }
    std::sort(status.catalog_buckets.begin(), status.catalog_buckets.end(),
              [](const CatalogBucketCount &a, const CatalogBucketCount &b) {
                return natural_less(a.name, b.name);
              });
  }

  return status;
}

std::string render_workspace_status(const WorkspaceStatus &st, bool verbose) {
  auto color_for = [&](ProblemStateKind k) -> std::string_view {
    switch (k) {
      case ProblemStateKind::Ac: return term::GREEN;
      case ProblemStateKind::Failed: return term::RED;
      case ProblemStateKind::Partial:
      case ProblemStateKind::Unrun:
      case ProblemStateKind::NoTests: return term::YELLOW;
      case ProblemStateKind::Untouched:
      case ProblemStateKind::LocalNoCph: return term::DIM;
    }
    return term::RESET;
  };

  auto format_badge = [&](const StatusProblemItem &item) -> std::string {
    std::string s;
    std::string_view col = color_for(item.state);
    s.append(col).push_back('[');
    s.append(item.verdict_tag).push_back(']');
    s.append(term::RESET);
    return s;
  };

  fs::path norm_ws = normalize_path(st.workspace_root);
  fs::path norm_cwd = normalize_path(st.cwd);

  std::string out;
  term::append_kv_row(out, "workspace root", st.workspace_root.string());
  if (norm_cwd != norm_ws) {
    term::append_kv_row(out, "cwd", rel_display(norm_ws, st.cwd));
  }
  if (st.latest_active_cpp) {
    term::append_kv_row(out, "latest cpp",
                        rel_display(norm_ws, *st.latest_active_cpp));
  } else {
    term::append_kv_row(out, "latest cpp", "(none)");
  }

  bool any_section = false;
  auto begin_section = [&](std::string_view title) {
    out.push_back('\n');
    out.append(title).push_back('\n');
    any_section = true;
  };

  if (!st.ready_to_stage.empty()) {
    begin_section("Ready to stage (" +
                  std::to_string(st.ready_to_stage.size()) + "):");
    for (const auto &item : st.ready_to_stage) {
      out.append("  ")
          .append(item.src_path.filename().string())
          .append(" -> ")
          .append(rel_display(norm_ws, *item.stage_dst))
          .append("  ")
          .append(format_badge(item))
          .push_back('\n');
    }
  }

  if (!st.root_contests.empty()) {
    begin_section("Root contests (" +
                  std::to_string(st.root_contests.size()) + "):");
    for (const auto &grp : st.root_contests) {
      size_t mod_cnt = grp.ac_count + grp.wip_count;
      out.append("  ")
          .append(grp.info.key)
          .append("  (")
          .append(std::to_string(mod_cnt))
          .append(" modified: ")
          .append(std::to_string(grp.ac_count))
          .append(" AC, ")
          .append(std::to_string(grp.wip_count))
          .append(" WIP; ")
          .append(std::to_string(grp.untouched_count))
          .append(" untouched)\n");
      for (const auto &item : grp.items) {
        if (item.state == ProblemStateKind::Untouched && !verbose) continue;
        out.append("    ")
            .append(item.src_path.filename().string())
            .append(" -> ")
            .append(item.label)
            .append(".cpp  ")
            .append(format_badge(item))
            .push_back('\n');
      }
      if (!verbose && grp.untouched_count > 0) {
        out.append("    ")
            .append(term::DIM)
            .append("+ ")
            .append(std::to_string(grp.untouched_count))
            .append(" untouched template(s) (use -v to list)")
            .append(term::RESET)
            .push_back('\n');
      }
    }
  }

  if (!st.subdir_workspaces.empty()) {
    begin_section("Subdir workspaces (" +
                  std::to_string(st.subdir_workspaces.size()) + "):");
    for (const auto &grp : st.subdir_workspaces) {
      out.append("  ")
          .append(grp.rel_name)
          .append("/  (")
          .append(std::to_string(grp.items.size()))
          .append(grp.items.size() == 1 ? " file: " : " files: ")
          .append(std::to_string(grp.ac_count))
          .append(" AC, ")
          .append(std::to_string(grp.wip_count))
          .append(" WIP");
      if (grp.untouched_count > 0) {
        out.append(", ")
            .append(std::to_string(grp.untouched_count))
            .append(" untouched");
      }
      if (grp.local_count > 0) {
        out.append(", ")
            .append(std::to_string(grp.local_count))
            .append(" local");
      }
      out.append(")\n");

      if (verbose) {
        for (const auto &item : grp.items) {
          out.append("    ")
              .append(item.label)
              .append("  ")
              .append(format_badge(item))
              .push_back('\n');
        }
      } else {
        out.append("    ");
        for (size_t i = 0; i < grp.items.size(); ++i) {
          if (i > 0) out.append(" | ");
          out.append(grp.items[i].label).push_back(' ');
          out.append(format_badge(grp.items[i]));
        }
        out.push_back('\n');
      }
    }
  }

  if (!st.in_progress_singles.empty()) {
    begin_section("In progress (" +
                  std::to_string(st.in_progress_singles.size()) + "):");
    for (const auto &item : st.in_progress_singles) {
      out.append("  ")
          .append(item.src_path.filename().string())
          .append("  ")
          .append(format_badge(item))
          .push_back('\n');
    }
  }

  if (!st.untouched_singles.empty()) {
    begin_section("Untouched templates (" +
                  std::to_string(st.untouched_singles.size()) + "):");
    out.append("  ");
    for (size_t i = 0; i < st.untouched_singles.size(); ++i) {
      if (i > 0) out.append(", ");
      out.append(st.untouched_singles[i].src_path.filename().string());
    }
    out.push_back('\n');
  }

  if (!st.local_root_files.empty()) {
    out.push_back('\n');
    any_section = true;
    out.append("Local files (no CPH): ");
    for (size_t i = 0; i < st.local_root_files.size(); ++i) {
      if (i > 0) out.append(", ");
      out.append(st.local_root_files[i].filename().string());
    }
    out.push_back('\n');
  }

  if (st.catalog_file_count > 0) {
    out.push_back('\n');
    any_section = true;
    out.append("Catalog (.wait): ")
        .append(std::to_string(st.catalog_file_count))
        .append(" file(s)");
    if (!st.catalog_buckets.empty()) {
      out.append(" (");
      for (size_t i = 0; i < st.catalog_buckets.size(); ++i) {
        if (i > 0) out.append(", ");
        out.append(st.catalog_buckets[i].name)
            .append(": ")
            .append(std::to_string(st.catalog_buckets[i].count));
      }
      out.push_back(')');
    }
    out.append(" -> ready for `move`\n");
  }

  if (st.total_unused_cph_count > 0 || st.orphan_bin_count > 0) {
    out.push_back('\n');
    any_section = true;
    out.append("Hygiene: ");
    bool first = true;
    if (st.total_unused_cph_count > 0) {
      out.append(std::to_string(st.total_unused_cph_count))
          .append(" unused CPH template(s) (`clean -u`)");
      first = false;
    }
    if (st.orphan_bin_count > 0) {
      if (!first) out.append(", ");
      out.append(std::to_string(st.orphan_bin_count))
          .append(" orphan .bin (`clean`)");
    }
    out.push_back('\n');
  }

  if (!any_section) {
    out.append("\nworkspace is clean\n");
  }

  if (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

}  // namespace yrs::cph

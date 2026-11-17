#include "archive.hpp"

#include "ui/term.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <sys/stat.h>

namespace yrs {

namespace {

Result<std::vector<MoveEntry>> move_dir_rec(const fs::path &cur_dir,
                                            const fs::path &catalog_root,
                                            const fs::path &record_root,
                                            const fs::path &summary_path) {
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
      auto sub =
          move_dir_rec(source_path, catalog_root, record_root, summary_path);
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

std::string_view branch(bool is_last) {
  return is_last ? "└── " : "├── ";
}

std::string child_prefix(std::string_view prefix, bool is_last) {
  std::string s(prefix);
  s.append(is_last ? "    " : "│   ");
  return s;
}

void render_terminal_dir(const MoveTree &dir, std::string_view prefix,
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
        (status == MoveEntryStatus::Created) ? term::GREEN : term::BLUE;
    std::string line(prefix);
    line.append(branch(is_last)).append(color).append(name).append(term::RESET);
    lines.push_back(std::move(line));
  }
}

void render_summary_dir(const MoveTree &dir, std::string_view prefix,
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

}  // namespace

Result<MoveSummary> move_catalog_into_record(const AppConfig &config) {
  std::error_code ec;
  if (!fs::exists(config.catalog_root, ec)) {
    return std::unexpected(make_err(
        ErrKind::NotFound,
        "missing catalog root: " + config.catalog_root.string()));
  }
  if (!fs::is_directory(config.catalog_root, ec)) {
    return std::unexpected(make_err(
        ErrKind::InvalidInput,
        "catalog root is not a dir: " + config.catalog_root.string()));
  }

  fs::create_directories(config.record_root, ec);
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));

  auto entries_res = move_dir_rec(config.catalog_root, config.catalog_root,
                                  config.record_root, config.summary_path);
  if (!entries_res) return std::unexpected(entries_res.error());

  auto entries = std::move(*entries_res);
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    return a.relative_path < b.relative_path;
  });
  return MoveSummary{std::move(entries)};
}

MoveDate MoveDate::from_unix_seconds(int64_t unix_seconds) {
  int64_t shifted = unix_seconds + 8 * 3600;
  int64_t days =
      shifted >= 0 ? (shifted / 86400) : ((shifted - 86399) / 86400);
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

std::string MoveDate::format(char sep) const {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04lld%c%02u%c%02u",
                static_cast<long long>(year), sep, month, sep, day);
  return buf;
}

void MoveTree::insert(const fs::path &relative_path, MoveEntryStatus status) {
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

std::vector<MoveDayGroup> build_move_day_groups(
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

std::string render_move_tree(const fs::path &record_root,
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

std::string render_grouped_move_trees(const fs::path &record_root,
                                      const std::vector<MoveDayGroup> &groups) {
  std::string out;
  for (size_t i = 0; i < groups.size(); ++i) {
    if (i > 0) out.append("\n\n");
    out.append(groups[i].date.format('-'));
    out.push_back('\n');
    out.append(render_move_tree(record_root, groups[i].tree));
  }
  return out;
}

std::string render_summary_tree(const MoveTree &tree) {
  std::vector<std::string> raw_lines;
  render_summary_dir(tree, "", raw_lines);
  std::string out;
  for (size_t i = 0; i < raw_lines.size(); ++i) {
    if (i > 0) out.push_back('\n');
    out.append("  ").append(raw_lines[i]);
  }
  return out;
}

std::string render_summary_history_blocks(
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

Result<void> update_summary_file(const AppConfig &config,
                                 const std::vector<MoveDayGroup> &groups) {
  if (groups.empty()) return {};

  std::string new_blocks = render::render_summary_history_blocks(groups);
  std::error_code ec;
  bool exists = fs::exists(config.summary_path, ec);
  if (ec) {
    return std::unexpected(make_err(ErrKind::Other, ec.message()));
  }
  if (exists) {
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

}  // namespace yrs

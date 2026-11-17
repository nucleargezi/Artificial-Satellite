#pragma once

#include "config.hpp"
#include "core/error.hpp"
#include "core/fs_util.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace yrs {

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

struct MoveDate {
  int64_t year;
  uint32_t month;
  uint32_t day;

  auto operator<=>(const MoveDate &) const = default;

  static MoveDate from_unix_seconds(int64_t unix_seconds);
  std::string format(char sep) const;
};

struct MoveTree {
  std::map<std::string, MoveTree> dirs;
  std::map<std::string, MoveEntryStatus> files;

  void insert(const fs::path &relative_path, MoveEntryStatus status);
};

struct MoveDayGroup {
  MoveDate date;
  MoveTree tree;
};

Result<MoveSummary> move_catalog_into_record(const AppConfig &config);
std::vector<MoveDayGroup> build_move_day_groups(
    const std::vector<MoveEntry> &entries);

namespace render {

std::string render_move_tree(const fs::path &record_root, const MoveTree &tree);
std::string render_grouped_move_trees(const fs::path &record_root,
                                      const std::vector<MoveDayGroup> &groups);
std::string render_summary_tree(const MoveTree &tree);
std::string render_summary_history_blocks(
    const std::vector<MoveDayGroup> &groups);

}  // namespace render

inline constexpr std::string_view HISTORY_HEADER_PREFIX =
    "#### History (since ";

Result<void> update_summary_file(const AppConfig &config,
                                 const std::vector<MoveDayGroup> &groups);

}  // namespace yrs

#pragma once

#include "config.hpp"
#include "core/error.hpp"
#include "core/fs_util.hpp"
#include "cph/meta.hpp"
#include "cph/oj.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yrs::cph {

struct StageEntry {
  fs::path src_path;
  fs::path dst_path;
  std::optional<std::string> url;
  bool bin_removed = false;
};

struct StageSummary {
  std::vector<StageEntry> entries;
};

struct UnusedProblemEntry {
  fs::path src_path;
  std::vector<fs::path> bin_paths;
};

struct CleanSummary {
  std::vector<fs::path> removed_bins;
  std::vector<UnusedProblemEntry> removed_unused;
};

struct ContestProblemItem {
  fs::path src_path;
  ProblemMeta meta;
  std::vector<fs::path> bin_paths;
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

enum class ProblemStateKind {
  Ac,
  Failed,
  Partial,
  Unrun,
  NoTests,
  Untouched,
  LocalNoCph,
};

struct StatusProblemItem {
  fs::path src_path;
  std::optional<fs::path> stage_dst;
  std::string label;
  ProblemStateKind state = ProblemStateKind::LocalNoCph;
  size_t enabled_tc = 0;
  size_t ac_tc = 0;
  std::string verdict_tag;
  bool unrouted_ac = false;
};

struct StatusContestGroup {
  ContestInfo info;
  std::vector<StatusProblemItem> items;
  size_t ac_count = 0;
  size_t wip_count = 0;
  size_t untouched_count = 0;
};

struct StatusSubdirGroup {
  fs::path dir_path;
  std::string rel_name;
  std::vector<StatusProblemItem> items;
  size_t ac_count = 0;
  size_t wip_count = 0;
  size_t untouched_count = 0;
  size_t local_count = 0;
};

struct CatalogBucketCount {
  std::string name;
  size_t count = 0;
};

struct WorkspaceStatus {
  fs::path workspace_root;
  fs::path cwd;
  std::optional<fs::path> latest_active_cpp;
  std::vector<StatusProblemItem> ready_to_stage;
  std::vector<StatusContestGroup> root_contests;
  std::vector<StatusSubdirGroup> subdir_workspaces;
  std::vector<StatusProblemItem> in_progress_singles;
  std::vector<StatusProblemItem> untouched_singles;
  std::vector<fs::path> local_root_files;
  size_t catalog_file_count = 0;
  std::vector<CatalogBucketCount> catalog_buckets;
  size_t orphan_bin_count = 0;
  size_t total_unused_cph_count = 0;
};

Result<void> stage_file_with_url(const fs::path &src, const fs::path &dst,
                                 std::optional<std::string_view> url);
Result<StageSummary> stage_single_problems(const AppConfig &config,
                                           bool dry_run);
Result<StageSummary> stage_directory(const AppConfig &config,
                                     const fs::path &src_dir_arg,
                                     const fs::path &wait_subdir_arg,
                                     bool dry_run);

Result<CleanSummary> clean_cph_workspace(const AppConfig &config, bool dry_run,
                                         bool include_unused = false);
Result<CleanSummary> clean_orphan_cph_bins(const AppConfig &config,
                                           bool dry_run);

Result<std::vector<ContestGroupSummary>> list_root_contests(
    const AppConfig &config);
Result<PackSummary> pack_contest(const AppConfig &config,
                                 std::optional<std::string_view> selector,
                                 const fs::path &contest_dir_arg, bool dry_run);

Result<WorkspaceStatus> inspect_workspace_status(const AppConfig &config,
                                                 const fs::path &cwd = {});
std::string render_workspace_status(const WorkspaceStatus &st, bool verbose);

}  // namespace yrs::cph

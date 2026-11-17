#pragma once

#include "config.hpp"
#include "core/error.hpp"
#include "core/fs_util.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yrs::cph {

struct ProblemMeta {
  fs::path bin_path;
  fs::path src_path;
  std::string name;
  std::optional<std::string> url;
  bool all_ac = false;
  size_t enabled_tc = 0;
  size_t ac_tc = 0;
  size_t failed_tc = 0;
  std::string first_fail_verdict;
};

struct IndexedProblem {
  ProblemMeta meta;
  std::vector<fs::path> bin_paths;
};

struct WorkspaceIndex {
  std::map<fs::path, IndexedProblem> by_source;
  std::vector<fs::path> orphan_bins;
};

Result<ProblemMeta> read_cph_bin(const fs::path &bin_path);
Result<void> relocate_cph_bin(const fs::path &old_bin, const fs::path &new_bin,
                              const fs::path &new_src_path);
bool is_untouched_template_content(std::string_view content,
                                   std::string_view tpl_content);
Result<WorkspaceIndex> scan_cph_index(const AppConfig &config);

}  // namespace yrs::cph

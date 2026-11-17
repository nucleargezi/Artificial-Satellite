#pragma once

#include "config.hpp"
#include "core/error.hpp"
#include "core/fs_util.hpp"

#include <optional>
#include <string>
#include <vector>

namespace yrs {

struct BundledLatest {
  fs::path source_path;
  std::string content;
  std::vector<fs::path> visited_local_files;
};

Result<std::optional<fs::path>> find_latest_active_cpp(
    const AppConfig &config, const fs::path &cwd = {});
Result<fs::path> cover_latest_cpp(const AppConfig &config,
                                  const fs::path &cwd = {},
                                  bool dry_run = false);
Result<BundledLatest> bundle_cpp_path(const AppConfig &config,
                                      const fs::path &source_path);
Result<BundledLatest> bundle_latest_cpp(const AppConfig &config,
                                        const fs::path &cwd = {});

}  // namespace yrs

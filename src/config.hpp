#pragma once

#include "core/error.hpp"
#include "core/fs_util.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace yrs {

struct AppConfig {
  fs::path workspace_root;
  fs::path library_root;
  fs::path catalog_root;
  fs::path record_root;
  fs::path summary_path;
  fs::path template_source;

  static AppConfig defaults_for_dir(const fs::path &dir);
  static Result<fs::path> restore_summary_path(const fs::path &record_root,
                                               const fs::path &summary_file);
  static Result<std::string> parse_toml_string(std::string_view raw,
                                               const fs::path &config_path);
  static Result<AppConfig> load_for_dir(const fs::path &dir);
  static Result<fs::path> find_workspace_root(const fs::path &start_dir);
  static Result<AppConfig> discover_for_dir(const fs::path &start_dir);
};

bool is_protected_workspace_dir(const AppConfig &config, const fs::path &dir);

struct WorkspaceRootScan {
  std::vector<fs::path> root_cpps;
  std::vector<fs::path> active_subdirs;
};

Result<WorkspaceRootScan> scan_workspace_root(const AppConfig &config);
Result<std::vector<fs::path>> collect_active_cpps_recursive(
    const AppConfig &config, const fs::path &root_dir);

}  // namespace yrs

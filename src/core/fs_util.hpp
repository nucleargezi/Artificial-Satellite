#pragma once

#include "core/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace yrs {

namespace fs = std::filesystem;

Result<std::string> read_file_to_string(const fs::path &p);
Result<void> write_string_to_file(const fs::path &p, std::string_view content);
Result<void> copy_file_atomic(const fs::path &from, const fs::path &to);
Result<void> move_file_across(const fs::path &from, const fs::path &to);

fs::path normalize_path(const fs::path &path);
bool path_starts_with(const fs::path &path, const fs::path &base);
std::string rel_display(const fs::path &norm_base, const fs::path &p);
void prune_empty_subdirs(const fs::path &root_dir, bool remove_root);

}  // namespace yrs

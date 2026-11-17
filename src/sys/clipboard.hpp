#pragma once

#include "core/error.hpp"

#include <string_view>
#include <vector>

namespace yrs::clipboard {

Result<void> run_clipboard_command(const char *program,
                                   const std::vector<const char *> &args,
                                   std::string_view content);
Result<void> copy_to_clipboard(std::string_view content);

}  // namespace yrs::clipboard

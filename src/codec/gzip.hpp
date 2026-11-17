#pragma once

#include "core/error.hpp"

#include <string>
#include <string_view>

namespace yrs::gzip {

Result<std::string> decompress(std::string_view input);
std::string compress_stored(std::string_view input);

}  // namespace yrs::gzip

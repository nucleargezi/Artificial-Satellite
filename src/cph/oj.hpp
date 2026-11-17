#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yrs::cph {

namespace fs = std::filesystem;

struct HttpUrl {
  std::string scheme;
  std::string host;
  std::string_view path;
  size_t path_offset = std::string_view::npos;
};

struct ContestInfo {
  std::string key;
  std::string short_id;
  std::string platform;
};

std::optional<HttpUrl> parse_http_url(std::string_view url);
bool host_matches(std::string_view host, std::string_view domain);
std::vector<std::string_view> split_path_segments(std::string_view path);
std::optional<std::string_view> find_query_param(std::string_view url,
                                                 std::string_view key);

std::string clean_problem_url(std::string_view url);
std::optional<fs::path> route_single_problem(const fs::path &catalog_root,
                                             std::string_view filename,
                                             std::string_view name,
                                             std::string_view url);
std::optional<ContestInfo> extract_contest_info(std::string_view url);
std::string extract_problem_label(std::string_view name, std::string_view url,
                                  std::string_view stem);
std::string prepend_url_comment_if_missing(std::string_view content,
                                           std::string_view url);

}  // namespace yrs::cph

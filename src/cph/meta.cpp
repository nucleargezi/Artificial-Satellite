#include "cph/meta.hpp"

#include "codec/gzip.hpp"
#include "codec/json.hpp"
#include "core/str_util.hpp"

#include <algorithm>
#include <cerrno>
#include <unistd.h>

namespace yrs::cph {

namespace {

std::string strip_template_prelude(std::string_view content) {
  std::string_view sv = content;
  if (sv.starts_with("\xEF\xBB\xBF")) sv.remove_prefix(3);

  auto nl = sv.find('\n');
  std::string_view first_line =
      (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
  std::string_view trimmed_first = trim_start(first_line);
  if (trimmed_first.starts_with("//") &&
      (trimmed_first.find("http://") != std::string_view::npos ||
       trimmed_first.find("https://") != std::string_view::npos)) {
    sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);
  }

  std::string clean;
  clean.reserve(sv.size());
  for (char c : sv) {
    if (c != '\r') clean.push_back(c);
  }
  return std::string(trim(clean));
}

}  // namespace

Result<ProblemMeta> read_cph_bin(const fs::path &bin_path) {
  auto raw = read_file_to_string(bin_path);
  if (!raw) return std::unexpected(raw.error());

  auto decompressed = gzip::decompress(*raw);
  if (!decompressed) {
    return std::unexpected(make_err(
        decompressed.error().kind,
        bin_path.string() + ": " + decompressed.error().message));
  }

  auto root = json::parse(*decompressed);
  if (!root) {
    return std::unexpected(make_err(
        root.error().kind, bin_path.string() + ": " + root.error().message));
  }
  if (!root->as_obj()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData,
                 bin_path.string() + ": root JSON must be an object"));
  }

  const json::Value *src_val = root->get("src");
  const json::Value *src_path_val = src_val ? src_val->get("path") : nullptr;
  auto src_path_str = src_path_val ? src_path_val->as_str() : std::nullopt;
  if (!src_path_str || src_path_str->empty()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData,
                 bin_path.string() + ": missing src.path"));
  }

  ProblemMeta meta;
  meta.bin_path = bin_path;
  meta.src_path = fs::path(*src_path_str);

  if (const json::Value *name_val = root->get("name")) {
    if (auto s = name_val->as_str()) meta.name = std::string(*s);
  }
  if (const json::Value *url_val = root->get("url")) {
    if (auto s = url_val->as_str(); s && !s->empty()) {
      meta.url = std::string(*s);
    }
  }

  if (const json::Value *tc_val = root->get("testcases")) {
    if (const json::Object *tcs = tc_val->as_obj()) {
      size_t enabled_cnt = 0;
      size_t ac_cnt = 0;
      size_t failed_cnt = 0;
      std::string first_fail;
      for (const auto &[id, tc] : tcs->entries) {
        (void)id;
        if (const json::Value *dis = tc.get("isDisabled")) {
          if (auto b = dis->as_bool(); b && *b) continue;
        }
        ++enabled_cnt;
        const json::Value *res = tc.get("result");
        if (res && !res->is_null()) {
          const json::Value *verdict = res->get("verdict");
          if (auto v_str = verdict ? verdict->as_str() : std::nullopt;
              v_str && !v_str->empty()) {
            if (*v_str == "AC") {
              ++ac_cnt;
            } else {
              ++failed_cnt;
              if (first_fail.empty()) first_fail = std::string(*v_str);
            }
          }
        }
      }
      meta.enabled_tc = enabled_cnt;
      meta.ac_tc = ac_cnt;
      meta.failed_tc = failed_cnt;
      meta.first_fail_verdict = std::move(first_fail);
      meta.all_ac = (enabled_cnt > 0 && ac_cnt == enabled_cnt);
    }
  }

  return meta;
}

Result<void> relocate_cph_bin(const fs::path &old_bin, const fs::path &new_bin,
                              const fs::path &new_src_path) {
  auto raw = read_file_to_string(old_bin);
  if (!raw) return std::unexpected(raw.error());

  auto decompressed = gzip::decompress(*raw);
  if (!decompressed) {
    return std::unexpected(make_err(
        decompressed.error().kind,
        old_bin.string() + ": " + decompressed.error().message));
  }

  auto root_res = json::parse(*decompressed);
  if (!root_res) {
    return std::unexpected(make_err(
        root_res.error().kind, old_bin.string() + ": " + root_res.error().message));
  }

  json::Value *src_val = root_res->get_mut("src");
  json::Value *path_val = src_val ? src_val->get_mut("path") : nullptr;
  if (!path_val || !path_val->as_str()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, old_bin.string() + ": missing src.path"));
  }
  path_val->data = new_src_path.string();

  std::string out_json = json::serialize(*root_res);
  std::string out_gz = gzip::compress_stored(out_json);

  std::error_code ec;
  if (new_bin.has_parent_path()) {
    fs::create_directories(new_bin.parent_path(), ec);
    if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
  }

  auto wr = write_string_to_file(new_bin, out_gz);
  if (!wr) return wr;

  if (normalize_path(old_bin) != normalize_path(new_bin)) {
    if (::unlink(old_bin.c_str()) != 0 && errno != ENOENT) {
      return std::unexpected(from_errno(old_bin.string()));
    }
  }
  return {};
}

bool is_untouched_template_content(std::string_view content,
                                   std::string_view tpl_content) {
  std::string norm_content = strip_template_prelude(content);
  if (norm_content.empty()) return true;
  std::string norm_tpl = strip_template_prelude(tpl_content);
  return !norm_tpl.empty() && norm_content == norm_tpl;
}

Result<WorkspaceIndex> scan_cph_index(const AppConfig &config) {
  WorkspaceIndex idx;
  fs::path cph_dir = config.workspace_root / ".cph-ng";
  std::error_code ec;
  if (!fs::is_directory(cph_dir, ec)) return idx;

  std::vector<fs::path> bin_files;
  for (fs::recursive_directory_iterator it(cph_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    auto st = it->symlink_status(ec);
    if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
    if (st.type() == fs::file_type::regular &&
        it->path().extension() == ".bin") {
      bin_files.push_back(it->path());
    }
  }
  if (ec) return std::unexpected(make_err(ErrKind::Other, ec.message()));
  std::sort(bin_files.begin(), bin_files.end());

  fs::path norm_ws = normalize_path(config.workspace_root);

  for (const auto &bin_path : bin_files) {
    auto meta_res = read_cph_bin(bin_path);
    if (!meta_res) return std::unexpected(meta_res.error());

    fs::path abs_src = meta_res->src_path.is_absolute()
                           ? meta_res->src_path
                           : (config.workspace_root / meta_res->src_path);
    fs::path norm_src = normalize_path(abs_src);

    if (path_starts_with(norm_src, norm_ws)) {
      auto src_st = fs::symlink_status(norm_src, ec);
      if (src_st.type() == fs::file_type::not_found) {
        ec.clear();
        idx.orphan_bins.push_back(bin_path);
        continue;
      }
      ec.clear();
    }

    auto &entry = idx.by_source[norm_src];
    entry.meta = std::move(*meta_res);
    entry.bin_paths.push_back(bin_path);
  }
  return idx;
}

}  // namespace yrs::cph

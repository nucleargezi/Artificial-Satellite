#include "yrs.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

namespace {

namespace fs = yrs::fs;

struct TempDir {
  fs::path path;

  TempDir() {
    std::string tpl = "/tmp/yrs_test_XXXXXX";
    char *res = ::mkdtemp(tpl.data());
    assert(res != nullptr);
    path = res;
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

fs::path write_file(const fs::path &dir, const std::string &name,
                    std::string_view content) {
  fs::path p = dir / name;
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  auto r = yrs::write_string_to_file(p, content);
  assert(r.has_value());
  return p;
}

void set_mtime(const fs::path &p, int64_t sec) {
  struct timespec ts[2];
  ts[0].tv_sec = sec;
  ts[0].tv_nsec = 0;
  ts[1].tv_sec = sec;
  ts[1].tv_nsec = 0;
  int rc = ::utimensat(AT_FDCWD, p.c_str(), ts, 0);
  assert(rc == 0);
}

void write_default_config(const fs::path &dir) {
  std::string cfg = "library_root = \"" + (dir / "YRS").string() +
                    "\"\n"
                    "catalog_root = \".wait\"\n"
                    "record_root = \"record\"\n"
                    "summary_file = \"summary.md\"\n";
  write_file(dir, "yrs.toml", cfg);
}

void test_config_load() {
  {
    TempDir tmp;
    std::string cfg = "library_root = \"" + (tmp.path / "lib").string() +
                      "\"\n"
                      "template_source = \"templates/base.cpp\"\n"
                      "catalog_root = \".wait\"\n"
                      "record_root = \"record\"\n"
                      "summary_file = \"summary.md\"\n";
    write_file(tmp.path, "yrs.toml", cfg);
    auto res = yrs::AppConfig::load_for_dir(tmp.path);
    assert(res.has_value());
    assert(res->workspace_root == tmp.path);
    assert(res->template_source == tmp.path / "templates/base.cpp");
    assert(res->catalog_root == tmp.path / ".wait");
    assert(res->record_root == tmp.path / "record");
    assert(res->summary_path == tmp.path / "record" / "summary.md");
    assert(res->library_root == tmp.path / "lib");
  }
  {
    TempDir tmp;
    write_default_config(tmp.path);
    auto res = yrs::AppConfig::load_for_dir(tmp.path);
    assert(res.has_value());
    assert(res->template_source == tmp.path / "0.cpp");
  }
  {
    TempDir tmp;
    auto res = yrs::AppConfig::load_for_dir(tmp.path);
    assert(!res.has_value());
    assert(res.error().kind == yrs::ErrKind::NotFound);
  }
  {
    TempDir tmp;
    write_file(tmp.path, "yrs.toml",
               "library_root = \"./rel\"\n"
               "catalog_root = \".wait\"\n"
               "record_root = \"record\"\n"
               "summary_file = \"summary.md\"\n");
    auto res = yrs::AppConfig::load_for_dir(tmp.path);
    assert(!res.has_value());
    assert(res.error().kind == yrs::ErrKind::InvalidData);
    assert(res.error().message == "library_root must be an absolute path");
  }
  {
    TempDir tmp;
    write_file(tmp.path, "yrs.toml",
               "library_root = \"" + (tmp.path / "lib").string() +
                   "\"\n"
                   "catalog_roots = [\".wait\"]\n"
                   "record_root = \"record\"\n"
                   "summary_file = \"summary.md\"\n");
    auto res = yrs::AppConfig::load_for_dir(tmp.path);
    assert(!res.has_value());
    assert(res.error().kind == yrs::ErrKind::InvalidData);
  }
  {
    TempDir tmp;
    write_file(tmp.path, "yrs.toml",
               "library_root = \"" + (tmp.path / "lib").string() +
                   "\"\n"
                   "catalog_root = \".wait\"\n"
                   "record_root = \"record\"\n"
                   "summary_file = \"nested/summary.md\"\n");
    auto res = yrs::AppConfig::load_for_dir(tmp.path);
    assert(!res.has_value());
    assert(res.error().kind == yrs::ErrKind::InvalidData);
  }
}

void test_latest_and_cover() {
  {
    TempDir tmp;
    write_default_config(tmp.path);
    auto tpl = write_file(tmp.path, "0.cpp", "template");
    set_mtime(tpl, 100);
    auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();
    assert(!yrs::find_latest_active_cpp(cfg).value().has_value());
    auto fi = write_file(tmp.path, "a.cpp", "");
    set_mtime(fi, 10);
    assert(yrs::normalize_path(*yrs::find_latest_active_cpp(cfg).value()) ==
           yrs::normalize_path(fi));
    auto se = write_file(tmp.path, "b.cpp", "");
    set_mtime(se, 20);
    assert(yrs::normalize_path(*yrs::find_latest_active_cpp(cfg).value()) ==
           yrs::normalize_path(se));
    auto sub = write_file(tmp.path / "sub", "c.cpp", "");
    set_mtime(sub, 30);
    assert(yrs::normalize_path(*yrs::find_latest_active_cpp(cfg).value()) ==
           yrs::normalize_path(sub));
  }
  {
    TempDir tmp;
    write_default_config(tmp.path);
    auto tpl = write_file(tmp.path, "0.cpp", "template body");
    set_mtime(tpl, 1);
    auto older = write_file(tmp.path, "a.cpp", "keep me");
    set_mtime(older, 5);
    auto newer = write_file(tmp.path, "b.cpp", "replace me");
    set_mtime(newer, 10);

    auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();
    auto covered = yrs::cover_latest_cpp(cfg).value();
    assert(covered == newer);
    assert(yrs::read_file_to_string(older).value() == "keep me");
    assert(yrs::read_file_to_string(newer).value() == "template body");
  }
}

void test_bundle() {
  {
    TempDir tmp;
    write_default_config(tmp.path);
    auto older = write_file(tmp.path, "0.cpp", "int old_main() { return 0; }\n");
    set_mtime(older, 1);
    auto latest = write_file(tmp.path, "main.cpp",
                             "#include <iostream>\n"
                             "#include \"a.hpp\"\n"
                             "#include \"a.hpp\"\n"
                             "\n"
                             "int main() {\n"
                             "  cout << \"Ciallo\" << endl;\n"
                             "  return 0;\n"
                             "}\n");
    set_mtime(latest, 10);
    write_file(tmp.path / "YRS", "a.hpp", "#pragma once\n#include \"b.hpp\"\n");
    write_file(tmp.path / "YRS", "b.hpp",
               "#pragma once\n#include <bits/stdc++.h>\n");

    auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();
    auto bundled = yrs::bundle_latest_cpp(cfg).value();
    assert(bundled.source_path == latest);
    assert(bundled.content ==
           "#include <iostream>\n"
           "#include <bits/stdc++.h>\n"
           "\n"
           "int main() {\n"
           "  cout << \"Ciallo\" << endl;\n"
           "  return 0;\n"
           "}\n");
  }
  {
    TempDir tmp;
    write_default_config(tmp.path);
    auto latest =
        write_file(tmp.path, "main.cpp",
                   "#include <vector>    \n"
                   "#include \"missing.hpp\" // keep this include\n"
                   "\n"
                   "int main() {    \n"
                   "  cout << \"http://example.com\"; // trim comment\n"
                   "  auto s = \"//\";    \n"
                   "\n"
                   "\n"
                   "  return 0;    \n"
                   "}\n");
    set_mtime(latest, 10);
    auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();
    auto bundled = yrs::bundle_latest_cpp(cfg).value();
    assert(bundled.content ==
           "#include <vector>\n"
           "#include \"missing.hpp\"\n"
           "\n"
           "int main() {\n"
           "  cout << \"http://example.com\";\n"
           "  auto s = \"//\";\n"
           "\n"
           "  return 0;\n"
           "}\n");
  }
  {
    TempDir tmp;
    write_default_config(tmp.path);
    auto selected = write_file(tmp.path, "selected.cpp",
                               "#include \"A/B/b.hpp\"\n"
                               "int chosen() {\n"
                               "  return 0;\n"
                               "}\n");
    write_file(tmp.path, "a.hpp", "int wrong() { return 7; }\n");
    write_file(tmp.path / "YRS" / "A" / "B", "b.hpp",
               "#include \"../a.hpp\"\n");
    write_file(tmp.path / "YRS" / "A", "a.hpp", "#include <iostream>\n");

    auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();
    auto bundled = yrs::bundle_cpp_path(cfg, selected).value();
    assert(bundled.content ==
           "#include <iostream>\n"
           "int chosen() {\n"
           "  return 0;\n"
           "}\n");
    assert(bundled.visited_local_files.size() == 2);
  }
}

void test_move_and_history() {
  TempDir tmp;
  write_default_config(tmp.path);
  write_file(tmp.path / "record" / "a", "stale.cpp", "old stale");

  auto late = write_file(tmp.path / ".wait", "late.cpp", "late");
  auto early = write_file(tmp.path / ".wait" / "a", "early.cpp", "early");
  auto stale = write_file(tmp.path / ".wait" / "a", "stale.cpp", "new stale");
  set_mtime(late, 1775057400);   // 2026-04-01 23:30 UTC+8
  set_mtime(early, 1775061000);  // 2026-04-02 00:30 UTC+8
  set_mtime(stale, 1775061000);  // 2026-04-02 00:30 UTC+8

  auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();
  auto mv = yrs::move_catalog_into_record(cfg).value();
  assert(mv.entries.size() == 3);
  assert(!fs::exists(late));
  assert(fs::exists(tmp.path / ".wait" / "a"));
  assert(yrs::read_file_to_string(tmp.path / "record" / "a" / "stale.cpp")
             .value() == "new stale");

  auto groups = yrs::build_move_day_groups(mv.entries);
  assert(groups.size() == 2);
  assert(groups[0].date.format('-') == "2026-04-02");
  assert(groups[1].date.format('-') == "2026-04-01");

  auto term = yrs::render::render_grouped_move_trees(cfg.record_root, groups);
  assert(term.find("2026-04-02\nrecord\n└── a\n    ├── "
                   "\x1b[32mearly.cpp\x1b[0m\n    └── "
                   "\x1b[94mstale.cpp\x1b[0m") != std::string::npos);
  assert(term.find("2026-04-01\nrecord\n└── \x1b[32mlate.cpp\x1b[0m") !=
         std::string::npos);

  assert(yrs::update_summary_file(cfg, groups).has_value());
  auto summary = yrs::read_file_to_string(cfg.summary_path).value();
  assert(summary.starts_with("#### History (since "));
  assert(summary.find("early.cpp | stale.cpp [cover]") != std::string::npos);
}

void test_paths_and_symlinks() {
  assert(yrs::path_starts_with("/a/b", "/a/b/"));
  assert(yrs::path_starts_with("/a/b/c", "/a/b/"));
  assert(!yrs::path_starts_with("/a/bc", "/a/b"));

  TempDir tmp;
  fs::path real_lib = tmp.path / "real_lib";
  fs::path link_lib = tmp.path / "link_lib";
  write_file(real_lib, "ok.hpp", "#pragma once\nint ok = 1;\n");
  auto outside = write_file(tmp.path, "outside.hpp", "int secret = 99;\n");

  assert(::symlink(real_lib.c_str(), link_lib.c_str()) == 0);
  assert(::symlink(outside.c_str(), (real_lib / "escape.hpp").c_str()) == 0);

  std::string cfg_str = "library_root = \"" + link_lib.string() +
                        "/\"\n"
                        "catalog_root = \".wait\"\n"
                        "record_root = \"record\"\n"
                        "summary_file = \"summary.md\"\n";
  write_file(tmp.path, "yrs.toml", cfg_str);
  auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();

  write_file(tmp.path, "sol.cpp",
             "#include \"ok.hpp\"\n"
             "#include \"escape.hpp\"\n"
             "int main() { return ok; }\n");
  fs::path subdir = tmp.path / "sub";
  fs::create_directories(subdir);

  auto orig_cwd = fs::current_path();
  fs::current_path(subdir);
  auto bundled = yrs::bundle_cpp_path(cfg, "../sol.cpp");
  fs::current_path(orig_cwd);

  assert(bundled.has_value());
  assert(bundled->content ==
         "int ok = 1;\n"
         "#include \"escape.hpp\"\n"
         "int main() { return ok; }\n");
}

void test_atomic_write_and_move() {
  TempDir tmp;
  auto target = write_file(tmp.path, "keep_perm.txt", "v1");
  assert(::chmod(target.c_str(), 0640) == 0);
  assert(yrs::write_string_to_file(target, "v2_atomic").has_value());
  assert(yrs::read_file_to_string(target).value() == "v2_atomic");
  struct stat sb{};
  assert(::stat(target.c_str(), &sb) == 0);
  assert((sb.st_mode & 0777) == 0640);

  fs::path dir_target = tmp.path / "adir";
  fs::create_directories(dir_target);
  assert(!yrs::write_string_to_file(dir_target, "fail").has_value());

  set_mtime(target, 1234567);
  fs::path copy_dst = tmp.path / "copied.txt";
  write_file(tmp.path, "copied.txt", "old");
  assert(yrs::copy_file_atomic(target, copy_dst).has_value());
  assert(yrs::read_file_to_string(copy_dst).value() == "v2_atomic");
  assert(::stat(copy_dst.c_str(), &sb) == 0);
  assert((sb.st_mode & 0777) == 0640);
  assert(sb.st_mtim.tv_sec == 1234567);

  for (const auto &entry : fs::directory_iterator(tmp.path)) {
    assert(!entry.path().filename().string().starts_with(".yrs_tmp."));
  }

  write_default_config(tmp.path);
  write_file(tmp.path, "yrs.toml",
             "library_root = \"" + (tmp.path / "YRS").string() +
                 "\"\n"
                 "bundle_output = \"out.cpp\"\n"
                 "catalog_root = \".wait\"\n"
                 "record_root = \"record\"\n"
                 "summary_file = \"summary.md\"\n");
  assert(!yrs::AppConfig::load_for_dir(tmp.path).has_value());

  write_default_config(tmp.path);
  auto src_f = write_file(tmp.path / ".wait", "clash", "file content");
  fs::create_directories(tmp.path / "record" / "clash");
  auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();
  assert(!yrs::move_catalog_into_record(cfg).has_value());
  assert(fs::exists(src_f));
}

void test_sigpipe_safety() {
  TempDir tmp;
  fs::path fail_bin = tmp.path / "early_exit.sh";
  write_file(tmp.path, "early_exit.sh",
             "#!/bin/sh\n"
             "exec <&-\n"
             "exit 42\n");
  assert(::chmod(fail_bin.c_str(), 0755) == 0);

  std::string large_payload(512 * 1024, 'A');
  auto res =
      yrs::clipboard::run_clipboard_command(fail_bin.c_str(), {}, large_payload);
  assert(!res.has_value());
  assert(res.error().message.find("exit status: 42") != std::string::npos);
}

void test_cli_integration(const char *cli_bin) {
  std::string abs_bin = fs::absolute(cli_bin).string();
  TempDir tmp;
  write_default_config(tmp.path);
  write_file(tmp.path / "YRS", "a.hpp", "#pragma once\nint x = 42;\n");
  write_file(tmp.path, "main.cpp", "#include \"a.hpp\"\nint main() { return x; }\n");

  fs::path bin_dir = tmp.path / "bin";
  fs::create_directories(bin_dir);
  fs::path wl_copy = bin_dir / "wl-copy";
  write_file(bin_dir, "wl-copy",
             "#!/bin/sh\n"
             "/bin/cat > \"$YRS_TEST_CLIPBOARD_CAPTURE\"\n"
             "/bin/sleep 2 &\n"
             "exit 0\n");
  ::chmod(wl_copy.c_str(), 0755);

  fs::path capture = tmp.path / "captured.txt";
  std::string cmd = "cd \"" + tmp.path.string() +
                    "\" && env -u DISPLAY -u WSL_DISTRO_NAME -u WSL_INTEROP "
                    "WAYLAND_DISPLAY=wayland-test PATH=\"" +
                    bin_dir.string() + "\" YRS_TEST_CLIPBOARD_CAPTURE=\"" +
                    capture.string() + "\" \"" + abs_bin +
                    "\" bundle >/dev/null 2>&1";
  auto t0 = std::time(nullptr);
  int rc = std::system(cmd.c_str());
  auto t1 = std::time(nullptr);
  assert(rc == 0);
  assert(t1 - t0 < 2);
  assert(yrs::read_file_to_string(capture).value() ==
         "int x = 42;\nint main() { return x; }\n");
}

std::string gzip_compress_via_system(const fs::path &tmp_dir,
                                      std::string_view input,
                                      const char *flags = "-n -9") {
  static int seq = 0;
  int id = ++seq;
  fs::path raw_path = tmp_dir / ("gz_in_" + std::to_string(id));
  fs::path gz_path = tmp_dir / ("gz_out_" + std::to_string(id) + ".gz");
  assert(yrs::write_string_to_file(raw_path, input).has_value());
  std::string cmd = std::string("gzip ") + flags + " -c \"" +
                    raw_path.string() + "\" > \"" + gz_path.string() + "\"";
  int rc = std::system(cmd.c_str());
  assert(rc == 0);
  auto out = yrs::read_file_to_string(gz_path);
  assert(out.has_value());
  std::error_code ec;
  fs::remove(raw_path, ec);
  fs::remove(gz_path, ec);
  return std::move(*out);
}

void test_gzip_and_json() {
  TempDir tmp;

  for (const char *flags : {"-n -1", "-n -9", "-9"}) {
    auto gz_empty = gzip_compress_via_system(tmp.path, "", flags);
    auto dec_empty = yrs::gzip::decompress(gz_empty);
    assert(dec_empty.has_value() && dec_empty->empty());
  }

  std::string sample_json =
      "{\"name\":\"P2075 \\u533a\\u95f4 LIS \\uD83D\\uDE00\","
      "\"ok\":true,\"skip\":false,\"num\":-12.5e2,\"nil\":null,"
      "\"arr\":[1,2,\"a\\n\\tb\\\\c\\/d\"]}";
  auto gz_json = gzip_compress_via_system(tmp.path, sample_json, "-9");
  auto dec_json = yrs::gzip::decompress(gz_json);
  assert(dec_json.has_value());
  assert(*dec_json == sample_json);

  auto parsed = yrs::json::parse(*dec_json);
  assert(parsed.has_value());
  assert(parsed->get("name")->as_str().value() ==
         "P2075 区间 LIS \xF0\x9F\x98\x80");
  assert(parsed->get("ok")->as_bool().value() == true);
  assert(parsed->get("skip")->as_bool().value() == false);
  assert(parsed->get("num")->as_num().value() == -1250.0);
  assert(parsed->get("nil")->is_null());
  assert(parsed->get("arr")->as_arr()->size() == 3);
  assert((*parsed->get("arr")->as_arr())[2].as_str().value() ==
         "a\n\tb\\c/d");

  // Large payload (> 64KB) with both repeated and pseudo-random segments
  std::string large;
  large.reserve(150000);
  uint32_t rng = 0x12345678u;
  for (int i = 0; i < 40000; ++i) {
    large.append("repeated_segment_");
  }
  for (int i = 0; i < 70000; ++i) {
    rng = rng * 1664525u + 1013904223u;
    large.push_back(static_cast<char>((rng >> 24) & 0xFFu));
  }
  for (const char *flags : {"-n -1", "-n -9"}) {
    auto gz_large = gzip_compress_via_system(tmp.path, large, flags);
    auto dec_large = yrs::gzip::decompress(gz_large);
    assert(dec_large.has_value());
    assert(*dec_large == large);
  }

  // Corrupt CRC32 and ISIZE and truncated stream
  std::string bad_crc = gz_json;
  bad_crc[bad_crc.size() - 8] ^= 0x01;
  assert(!yrs::gzip::decompress(bad_crc).has_value());

  std::string bad_isize = gz_json;
  bad_isize[bad_isize.size() - 1] ^= 0x01;
  assert(!yrs::gzip::decompress(bad_isize).has_value());

  assert(!yrs::gzip::decompress(gz_json.substr(0, gz_json.size() - 5))
              .has_value());
  assert(!yrs::json::parse("{\"a\": [1, 2}").has_value());
  assert(!yrs::json::parse("\"unterminated").has_value());
}

void write_cph_bin(const TempDir &tmp, const std::string &bin_rel,
                   const fs::path &src_path, std::string_view name,
                   std::optional<std::string_view> url,
                   std::string_view testcases_json) {
  std::string json = "{\"version\":\"0.7.13\",\"name\":\"";
  json.append(name).append("\",\"url\":");
  if (url) {
    json.append("\"").append(*url).append("\"");
  } else {
    json.append("null");
  }
  json.append(",\"testcases\":").append(testcases_json);
  json.append(",\"src\":{\"path\":\"")
      .append(src_path.string())
      .append("\",\"hash\":\"abc\"}}");
  std::string gz = gzip_compress_via_system(tmp.path, json, "-n -9");
  write_file(tmp.path / ".cph-ng", bin_rel, gz);
}

void test_cph_stage_and_clean(const char *cli_bin) {
  TempDir tmp;
  write_default_config(tmp.path);
  write_file(tmp.path, "0.cpp", "// template\nint main() {}\n");

  std::string tc_all_ac =
      "{\"1\":{\"isDisabled\":false,\"result\":{\"verdict\":\"AC\"}},"
      "\"2\":{\"isDisabled\":false,\"result\":{\"verdict\":\"AC\"}}}";
  std::string tc_disabled_wa_enabled_ac =
      "{\"1\":{\"isDisabled\":true,\"result\":{\"verdict\":\"WA\"}},"
      "\"2\":{\"isDisabled\":false,\"result\":{\"verdict\":\"AC\"}}}";
  std::string tc_wa =
      "{\"1\":{\"isDisabled\":false,\"result\":{\"verdict\":\"WA\"}}}";
  std::string tc_unrun =
      "{\"1\":{\"isDisabled\":false,\"result\":null}}";
  std::string tc_all_disabled =
      "{\"1\":{\"isDisabled\":true,\"result\":{\"verdict\":\"AC\"}}}";

  auto p2075 = write_file(tmp.path, "P2075.cpp", "int main() { return 0; }\n");
  set_mtime(p2075, 1775050000);
  write_cph_bin(tmp, "000_dup_old_p2075.bin", p2075, "P2075 old",
                "https://www.luogu.com.cn/problem/P2075", tc_wa);
  write_cph_bin(tmp, "P2075.cpp.bin", p2075, "P2075 区间 LIS",
                "https://www.luogu.com.cn/problem/P2075", tc_all_ac);

  auto b2104 = write_file(tmp.path, "B2104.cpp",
                          "// https://www.luogu.com.cn/problem/B2104\n"
                          "int main() { return 1; }\n");
  set_mtime(b2104, 1775050100);
  write_cph_bin(tmp, "B2104.cpp.bin", b2104, "B2104",
                "https://www.luogu.com.cn/problem/B2104",
                tc_disabled_wa_enabled_ac);

  auto lc = write_file(tmp.path, "Point_Add_Range_Sum.cpp", "int lc() {}\n");
  set_mtime(lc, 1775050200);
  write_cph_bin(tmp, "Point_Add_Range_Sum.cpp.bin", lc, "Point Add Range Sum",
                "https://judge.yosupo.jp/problem/point_add_range_sum?lang=ja",
                tc_all_ac);

  auto loj = write_file(tmp.path, "180_正则二分图匹配.cpp", "int loj() {}\n");
  set_mtime(loj, 1775050300);
  write_cph_bin(tmp, "180_正则二分图匹配.cpp.bin", loj, "180",
                "https://loj.ac/p/180", tc_all_ac);

  auto uoj = write_file(tmp.path, "106_动态仙人掌_IV.cpp", "int uoj() {}\n");
  set_mtime(uoj, 1775050400);
  write_cph_bin(tmp, "106_动态仙人掌_IV.cpp.bin", uoj, "106",
                "https://uoj.ac/problem/106", tc_all_ac);

  auto yk15 = write_file(tmp.path, "No_15_test.cpp", "int y1() {}\n");
  set_mtime(yk15, 1775050500);
  write_cph_bin(tmp, "No_15_test.cpp.bin", yk15, "No.15 Test",
                "https://yukicoder.me/problems/no/15", tc_all_ac);

  auto yk100 = write_file(tmp.path, "No_100_test.cpp", "int y2() {}\n");
  set_mtime(yk100, 1775050600);
  write_cph_bin(tmp, "No_100_test.cpp.bin", yk100, "No.100 Test",
                "https://yukicoder.me/problems/44", tc_all_ac);

  // Should be skipped by stage_single_problems
  auto qoj = write_file(tmp.path, "qoj8340.cpp", "int q() {}\n");
  write_cph_bin(tmp, "qoj8340.cpp.bin", qoj, "M. 3 Sum",
                "https://qoj.ac/contest/1540/problem/8340/statement/zh_cn",
                tc_all_ac);

  auto cf = write_file(tmp.path, "142E.cpp", "int c() {}\n");
  write_cph_bin(tmp, "142E.cpp.bin", cf, "142E",
                "https://codeforces.com/contest/142/problem/E", tc_all_ac);

  auto at = write_file(tmp.path, "abc395f.cpp", "int a() {}\n");
  write_cph_bin(tmp, "abc395f.cpp.bin", at, "F",
                "https://atcoder.jp/contests/abc395/tasks/abc395_f", tc_all_ac);

  auto wa_file = write_file(tmp.path, "P9999.cpp", "int w() {}\n");
  write_cph_bin(tmp, "P9999.cpp.bin", wa_file, "P9999",
                "https://www.luogu.com.cn/problem/P9999", tc_wa);

  auto unrun_file = write_file(tmp.path, "P8888.cpp", "int u() {}\n");
  write_cph_bin(tmp, "P8888.cpp.bin", unrun_file, "P8888",
                "https://www.luogu.com.cn/problem/P8888", tc_unrun);

  auto dis_file = write_file(tmp.path, "P7777.cpp", "int d() {}\n");
  write_cph_bin(tmp, "P7777.cpp.bin", dis_file, "P7777",
                "https://www.luogu.com.cn/problem/P7777", tc_all_disabled);

  auto no_url = write_file(tmp.path, "J.cpp.cpp", "int j() {}\n");
  write_cph_bin(tmp, "J.cpp.cpp.bin", no_url, "J.cpp", std::nullopt,
                tc_all_ac);

  auto elf_dummy = write_file(tmp.path, "E", "\x7f" "ELF_DUMMY");

  auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();

  // Dry-run must not modify files
  auto dry_stage = yrs::cph::stage_single_problems(cfg, true).value();
  assert(dry_stage.entries.size() == 7);
  assert(fs::exists(p2075));
  assert(fs::exists(tmp.path / ".cph-ng" / "000_dup_old_p2075.bin"));
  assert(fs::exists(tmp.path / ".cph-ng" / "P2075.cpp.bin"));
  assert(!fs::exists(tmp.path / ".wait" / "luogu" / "P2075.cpp"));

  // Real stage
  auto real_stage = yrs::cph::stage_single_problems(cfg, false).value();
  assert(real_stage.entries.size() == 7);
  assert(!fs::exists(p2075));
  assert(!fs::exists(tmp.path / ".cph-ng" / "000_dup_old_p2075.bin"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "P2075.cpp.bin"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "B2104.cpp.bin"));

  fs::path staged_p2075 = tmp.path / ".wait" / "luogu" / "P2075.cpp";
  assert(yrs::read_file_to_string(staged_p2075).value() ==
         "// https://www.luogu.com.cn/problem/P2075\n"
         "int main() { return 0; }\n");
  struct stat sb{};
  assert(::stat(staged_p2075.c_str(), &sb) == 0);
  assert(sb.st_mtim.tv_sec == 1775050000);

  fs::path staged_b2104 = tmp.path / ".wait" / "luogu" / "B" / "B2104.cpp";
  assert(yrs::read_file_to_string(staged_b2104).value() ==
         "// https://www.luogu.com.cn/problem/B2104\n"
         "int main() { return 1; }\n");
  assert(::stat(staged_b2104.c_str(), &sb) == 0);
  assert(sb.st_mtim.tv_sec == 1775050100);

  assert(fs::exists(tmp.path / ".wait" / "library_checker" /
                    "Point_Add_Range_Sum.cpp"));
  assert(fs::exists(tmp.path / ".wait" / "loj" / "180_正则二分图匹配.cpp"));
  assert(fs::exists(tmp.path / ".wait" / "uoj" / "106_动态仙人掌_IV.cpp"));
  assert(fs::exists(tmp.path / ".wait" / "yukicoder" / "0001-0100" /
                    "No_15_test.cpp"));
  assert(fs::exists(tmp.path / ".wait" / "yukicoder" / "0100-0200" /
                    "No_100_test.cpp"));

  // Skipped files and their .bins must still exist
  assert(fs::exists(qoj));
  assert(fs::exists(cf));
  assert(fs::exists(at));
  assert(fs::exists(wa_file));
  assert(fs::exists(unrun_file));
  assert(fs::exists(dis_file));
  assert(fs::exists(no_url));
  assert(fs::exists(elf_dummy));
  assert(fs::exists(tmp.path / ".cph-ng" / "qoj8340.cpp.bin"));

  // Contest directory stage
  auto ca = write_file(tmp.path / "2026_contest", "A.cpp", "int ca() {}\n");
  auto cb = write_file(tmp.path / "2026_contest", "B.cpp", "int cb() {}\n");
  set_mtime(ca, 1775060000);
  set_mtime(cb, 1775060100);
  write_cph_bin(tmp, "2026_contest/A.cpp.bin", ca, "A. Alpha",
                "https://codeforces.com/contest/999/problem/A", tc_unrun);

  assert(!yrs::cph::stage_directory(cfg, ".", "bad", false).has_value());
  assert(!yrs::cph::stage_directory(cfg, "2026_contest", "../escape", false)
              .has_value());

  auto dry_dir =
      yrs::cph::stage_directory(cfg, "2026_contest", "regional/cn/2026/contest",
                                true)
          .value();
  assert(dry_dir.entries.size() == 2);
  assert(fs::exists(ca));

  auto real_dir =
      yrs::cph::stage_directory(cfg, "2026_contest", "regional/cn/2026/contest",
                                false)
          .value();
  assert(real_dir.entries.size() == 2);
  assert(!fs::exists(tmp.path / "2026_contest"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "2026_contest"));

  fs::path dst_ca = tmp.path / ".wait" / "regional" / "cn" / "2026" /
                    "contest" / "A.cpp";
  fs::path dst_cb = tmp.path / ".wait" / "regional" / "cn" / "2026" /
                    "contest" / "B.cpp";
  assert(yrs::read_file_to_string(dst_ca).value() ==
         "// https://codeforces.com/contest/999/problem/A\n"
         "int ca() {}\n");
  assert(yrs::read_file_to_string(dst_cb).value() == "int cb() {}\n");
  assert(::stat(dst_ca.c_str(), &sb) == 0 && sb.st_mtim.tv_sec == 1775060000);
  assert(::stat(dst_cb.c_str(), &sb) == 0 && sb.st_mtim.tv_sec == 1775060100);

  // Orphan .bin cleaning
  fs::path outside_src = write_file(tmp.path / "outside_dir", "ext.cpp", "");
  fs::path external_bin_target = "/tmp/yrs_nonexistent_outside_12345.cpp";
  write_cph_bin(tmp, "orphan.cpp.bin", tmp.path / "orphan.cpp", "Orphan",
                std::nullopt, tc_all_ac);
  write_cph_bin(tmp, "sub/gone.cpp.bin", tmp.path / "sub" / "gone.cpp", "Gone",
                std::nullopt, tc_all_ac);
  write_cph_bin(tmp, "outside.cpp.bin", external_bin_target, "Outside",
                std::nullopt, tc_all_ac);
  (void)outside_src;

  auto dry_clean = yrs::cph::clean_orphan_cph_bins(cfg, true).value();
  assert(dry_clean.removed_bins.size() == 2);
  assert(fs::exists(tmp.path / ".cph-ng" / "orphan.cpp.bin"));
  assert(fs::exists(tmp.path / ".cph-ng" / "sub" / "gone.cpp.bin"));

  // CLI move should archive .wait and automatically clean orphan .bins
  if (cli_bin != nullptr) {
    std::string abs_bin = fs::absolute(cli_bin).string();
    std::string bad_stage = "cd \"" + tmp.path.string() + "\" && \"" + abs_bin +
                            "\" stage only_one >/dev/null 2>&1";
    assert(std::system(bad_stage.c_str()) != 0);

    std::string bad_clean = "cd \"" + tmp.path.string() + "\" && \"" + abs_bin +
                            "\" clean extra >/dev/null 2>&1";
    assert(std::system(bad_clean.c_str()) != 0);

    std::string mv_cmd = "cd \"" + tmp.path.string() + "\" && \"" + abs_bin +
                         "\" move >/dev/null 2>&1";
    assert(std::system(mv_cmd.c_str()) == 0);
  } else {
    assert(yrs::move_catalog_into_record(cfg).has_value());
    auto cleaned = yrs::cph::clean_orphan_cph_bins(cfg, false).value();
    assert(cleaned.removed_bins.size() == 2);
  }

  assert(!fs::exists(tmp.path / ".cph-ng" / "orphan.cpp.bin"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "sub"));
  assert(fs::exists(tmp.path / ".cph-ng" / "outside.cpp.bin"));
  assert(fs::exists(tmp.path / ".cph-ng" / "qoj8340.cpp.bin"));
  assert(fs::exists(elf_dummy));
  assert(fs::exists(tmp.path / "record" / "luogu" / "P2075.cpp"));
}

void test_cph_pack(const char *cli_bin) {
  TempDir tmp;

  // 1. Test gzip::compress_stored + json::serialize + system gzip -t
  {
    for (std::string_view payload :
         {std::string_view(""),
          std::string_view("{\"a\":123,\"b\":true,\"s\":\"hello\\n世界\"}")}) {
      std::string gz = yrs::gzip::compress_stored(payload);
      fs::path gz_file = tmp.path / "stored_test.gz";
      assert(yrs::write_string_to_file(gz_file, gz).has_value());
      std::string tcmd = "gzip -t \"" + gz_file.string() + "\"";
      assert(std::system(tcmd.c_str()) == 0);
      auto dec = yrs::gzip::decompress(gz);
      assert(dec.has_value() && *dec == payload);
    }
    std::string big(140000, 'Z');
    big[70000] = 'Q';
    std::string gz_big = yrs::gzip::compress_stored(big);
    fs::path gz_big_file = tmp.path / "stored_big.gz";
    assert(yrs::write_string_to_file(gz_big_file, gz_big).has_value());
    assert(std::system(("gzip -t \"" + gz_big_file.string() + "\"").c_str()) == 0);
    auto dec_big = yrs::gzip::decompress(gz_big);
    assert(dec_big.has_value() && *dec_big == big);
  }

  write_default_config(tmp.path);
  write_file(tmp.path, "0.cpp",
             "#include \"YRS/all.hpp\"\nvoid Yorisou() {\n}\nint main() {}\n");

  std::string tc_all_ac =
      "{\"1\":{\"isDisabled\":false,\"result\":{\"verdict\":\"AC\"}}}";
  std::string tc_wa =
      "{\"1\":{\"isDisabled\":false,\"result\":{\"verdict\":\"WA\"}}}";

  // Contest 1: qoj:1540 (2 modified including 1 WA, 1 untouched template)
  auto q_a = write_file(tmp.path, "qoj8328.cpp", "int a_sol() { return 1; }\n");
  set_mtime(q_a, 1775070000);
  write_cph_bin(tmp, "000_dup_q8328.bin", q_a, "A. old",
                "https://qoj.ac/contest/1540/problem/8328/statement/zh_cn",
                tc_wa);
  write_cph_bin(tmp, "qoj8328.cpp.bin", q_a, "A. 一道好题",
                "https://qoj.ac/contest/1540/problem/8328/statement/zh_cn",
                tc_all_ac);

  auto q_b = write_file(
      tmp.path, "qoj8329.cpp",
      "#include \"YRS/all.hpp\"\r\nvoid Yorisou() {\r\n}\r\nint main() {}\r\n");
  set_mtime(q_b, 1775070100);
  write_cph_bin(tmp, "000_dup_q8329.bin", q_b, "B. old",
                "https://qoj.ac/contest/1540/problem/8329/statement/zh_cn",
                tc_wa);
  write_cph_bin(tmp, "qoj8329.cpp.bin", q_b, "B. 空题",
                "https://qoj.ac/contest/1540/problem/8329/statement/zh_cn",
                tc_wa);

  auto q_m = write_file(tmp.path, "qoj8340.cpp", "int m_sol() { return 2; }\n");
  set_mtime(q_m, 1775070200);
  write_cph_bin(tmp, "qoj8340.cpp.bin", q_m, "M. 3 Sum",
                "https://qoj.ac/contest/1540/problem/8340/statement/zh_cn",
                tc_wa);

  // Contest 2: cf:2050 (label fallback from URL because title is "Count Subarrays")
  auto cf_a = write_file(tmp.path, "2050A.cpp", "int cf_sol() { return 3; }\n");
  set_mtime(cf_a, 1775070300);
  write_cph_bin(tmp, "2050A.cpp.bin", cf_a, "Count Subarrays",
                "https://codeforces.com/contest/2050/problem/A", tc_wa);

  // Unrelated files that must never be touched
  auto luogu_p = write_file(tmp.path, "P2075.cpp", "int p2075() {}\n");
  write_cph_bin(tmp, "P2075.cpp.bin", luogu_p, "P2075",
                "https://www.luogu.com.cn/problem/P2075", tc_all_ac);
  auto gen_f = write_file(tmp.path, "gen.cpp", "int gen() {}\n");

  auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();

  auto groups = yrs::cph::list_root_contests(cfg).value();
  assert(groups.size() == 2);

  // Multiple contests without selector must error
  auto ambig = yrs::cph::pack_contest(cfg, std::nullopt, "shenzhen", false);
  assert(!ambig.has_value());
  assert(ambig.error().kind == yrs::ErrKind::InvalidInput);

  // Dry-run leaves disk untouched
  auto dry_pack =
      yrs::cph::pack_contest(cfg, "1540", "2023 ccpc shenzhen", true).value();
  assert(dry_pack.packed_files.size() == 2);
  assert(dry_pack.dropped_templates.size() == 1);
  assert(fs::exists(q_a) && fs::exists(q_b) && fs::exists(q_m));
  assert(!fs::exists(tmp.path / "2023 ccpc shenzhen"));

  // Conflict pre-check: existing target blocks everything atomically
  auto clash = write_file(tmp.path / "2023 ccpc shenzhen", "A.cpp", "existing");
  auto clash_res =
      yrs::cph::pack_contest(cfg, "1540", "2023 ccpc shenzhen", false);
  assert(!clash_res.has_value());
  assert(fs::exists(q_a) && fs::exists(q_b) && fs::exists(q_m));
  assert(fs::exists(tmp.path / ".cph-ng" / "qoj8329.cpp.bin"));
  fs::remove(clash);

  // Real pack for 1540
  auto real_pack =
      yrs::cph::pack_contest(cfg, "1540", "2023 ccpc shenzhen", false).value();
  assert(real_pack.packed_files.size() == 2);
  assert(real_pack.dropped_templates.size() == 1);

  fs::path dst_a = tmp.path / "2023 ccpc shenzhen" / "A.cpp";
  fs::path dst_m = tmp.path / "2023 ccpc shenzhen" / "M.cpp";
  assert(fs::exists(dst_a) && fs::exists(dst_m));
  assert(!fs::exists(tmp.path / "2023 ccpc shenzhen" / "B.cpp"));
  assert(!fs::exists(q_a) && !fs::exists(q_b) && !fs::exists(q_m));
  assert(!fs::exists(tmp.path / ".cph-ng" / "000_dup_q8328.bin"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "000_dup_q8329.bin"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "qoj8328.cpp.bin"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "qoj8329.cpp.bin"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "qoj8340.cpp.bin"));

  assert(yrs::read_file_to_string(dst_a).value() ==
         "// https://qoj.ac/contest/1540/problem/8328\n"
         "int a_sol() { return 1; }\n");
  assert(yrs::read_file_to_string(dst_m).value() ==
         "// https://qoj.ac/contest/1540/problem/8340\n"
         "int m_sol() { return 2; }\n");

  struct stat sb{};
  assert(::stat(dst_a.c_str(), &sb) == 0 && sb.st_mtim.tv_sec == 1775070000);
  assert(::stat(dst_m.c_str(), &sb) == 0 && sb.st_mtim.tv_sec == 1775070200);

  fs::path new_bin_a =
      tmp.path / ".cph-ng" / "2023 ccpc shenzhen" / "A.cpp.bin";
  fs::path new_bin_m =
      tmp.path / ".cph-ng" / "2023 ccpc shenzhen" / "M.cpp.bin";
  assert(fs::exists(new_bin_a) && fs::exists(new_bin_m));
  auto meta_a = yrs::cph::read_cph_bin(new_bin_a).value();
  auto meta_m = yrs::cph::read_cph_bin(new_bin_m).value();
  assert(meta_a.src_path == dst_a && meta_a.all_ac == true);
  assert(meta_m.src_path == dst_m && meta_m.all_ac == false);

  // Unrelated files untouched
  assert(fs::exists(cf_a) && fs::exists(luogu_p) && fs::exists(gen_f));

  // Explicit bundle <subpath/file.cpp> via CLI + stage_directory end-to-end
  if (cli_bin != nullptr) {
    std::string abs_bin = fs::absolute(cli_bin).string();

    write_file(tmp.path / "YRS", "lib.hpp", "#pragma once\nint libv = 7;\n");
    write_file(tmp.path / "2023 ccpc shenzhen", "X.cpp",
               "#include \"lib.hpp\"\nint x() { return libv; }\n");
    fs::path bin_dir = tmp.path / "bin";
    fs::create_directories(bin_dir);
    fs::path wl_copy = bin_dir / "wl-copy";
    write_file(bin_dir, "wl-copy",
               "#!/bin/sh\n/bin/cat > \"$YRS_TEST_CLIPBOARD_CAPTURE\"\n");
    ::chmod(wl_copy.c_str(), 0755);

    fs::path cap = tmp.path / "bundle_sub.txt";
    std::string bcmd = "cd \"" + tmp.path.string() +
                       "\" && env -u DISPLAY -u WSL_DISTRO_NAME -u WSL_INTEROP "
                       "WAYLAND_DISPLAY=wayland-test PATH=\"" +
                       bin_dir.string() + "\" YRS_TEST_CLIPBOARD_CAPTURE=\"" +
                       cap.string() + "\" \"" + abs_bin +
                       "\" bundle \"2023 ccpc shenzhen/X.cpp\" >/dev/null 2>&1";
    assert(std::system(bcmd.c_str()) == 0);
    assert(yrs::read_file_to_string(cap).value() ==
           "int libv = 7;\nint x() { return libv; }\n");
    fs::remove(tmp.path / "2023 ccpc shenzhen" / "X.cpp");

    // Pack remaining single contest without <CONTEST_ID> via CLI
    std::string pcmd = "cd \"" + tmp.path.string() + "\" && \"" + abs_bin +
                       "\" pack cf_2050 >/dev/null 2>&1";
    assert(std::system(pcmd.c_str()) == 0);
    assert(fs::exists(tmp.path / "cf_2050" / "A.cpp"));
    assert(fs::exists(tmp.path / ".cph-ng" / "cf_2050" / "A.cpp.bin"));
    assert(!fs::exists(cf_a));
  }

  // Stage packed contest into .wait/regional/cn/2023/shenzhen
  auto stg = yrs::cph::stage_directory(
                 cfg, "2023 ccpc shenzhen", "regional/cn/2023/shenzhen", false)
                 .value();
  assert(stg.entries.size() == 2);
  assert(!fs::exists(tmp.path / "2023 ccpc shenzhen"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "2023 ccpc shenzhen"));
  assert(fs::exists(tmp.path / ".wait" / "regional" / "cn" / "2023" /
                    "shenzhen" / "A.cpp"));
  assert(fs::exists(tmp.path / ".wait" / "regional" / "cn" / "2023" /
                    "shenzhen" / "M.cpp"));
}

void test_config_discover() {
  TempDir tmp;
  write_default_config(tmp.path);
  fs::path nested = tmp.path / "2023 ccpc shenzhen" / "sub";
  fs::create_directories(nested);

  auto found = yrs::AppConfig::discover_for_dir(nested);
  assert(found.has_value());
  assert(found->workspace_root == yrs::normalize_path(tmp.path));
  assert(found->catalog_root == yrs::normalize_path(tmp.path) / ".wait");

  TempDir no_cfg;
  fs::path orphan_sub = no_cfg.path / "a" / "b";
  fs::create_directories(orphan_sub);
  if (!fs::exists("/yrs.toml") && !fs::exists("/tmp/yrs.toml")) {
    auto missing = yrs::AppConfig::discover_for_dir(orphan_sub);
    assert(!missing.has_value());
    assert(missing.error().kind == yrs::ErrKind::NotFound);
  }
}

void test_subdir_latest_and_cover(const char *cli_bin) {
  TempDir tmp;
  write_default_config(tmp.path);
  auto tpl = write_file(tmp.path, "0.cpp", "// tpl\nint main() { return 0; }\n");
  set_mtime(tpl, 9999);
  write_file(tmp.path / "YRS", "lib.hpp", "#pragma once\nint lv = 11;\n");

  auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();

  // Only template_source exists -> must never pick 0.cpp
  assert(!yrs::find_latest_active_cpp(cfg, tmp.path).value().has_value());
  assert(!yrs::bundle_latest_cpp(cfg, tmp.path).has_value());
  assert(!yrs::cover_latest_cpp(cfg, tmp.path).has_value());
  assert(yrs::read_file_to_string(tpl).value() ==
         "// tpl\nint main() { return 0; }\n");

  auto root_a = write_file(tmp.path, "a.cpp", "int a() { return 1; }\n");
  set_mtime(root_a, 10);
  assert(yrs::normalize_path(
             *yrs::find_latest_active_cpp(cfg, tmp.path).value()) ==
         yrs::normalize_path(root_a));

  auto ca = write_file(tmp.path / "contest_a", "A.cpp", "int ca() {}\n");
  set_mtime(ca, 40);
  auto cb = write_file(tmp.path / "contest_b", "M.cpp",
                       "#include \"lib.hpp\"\nint cm() { return lv; }\n");
  set_mtime(cb, 60);

  // Newer files inside protected / hidden dirs must be strictly ignored
  set_mtime(write_file(tmp.path / ".wait", "w.cpp", "int w() {}\n"), 500);
  set_mtime(write_file(tmp.path / "record", "r.cpp", "int r() {}\n"), 600);
  set_mtime(write_file(tmp.path / "YRS", "y.cpp", "int y() {}\n"), 700);
  set_mtime(write_file(tmp.path / ".hidden", "h.cpp", "int h() {}\n"), 800);
  set_mtime(write_file(tmp.path / "contest_b" / ".git", "g.cpp", "int g() {}\n"),
            900);

  // From workspace root: picks newest active file across subdirs (contest_b/M.cpp)
  auto latest_root = yrs::find_latest_active_cpp(cfg, tmp.path).value();
  assert(latest_root.has_value());
  assert(yrs::normalize_path(*latest_root) == yrs::normalize_path(cb));

  // From contest_a subdir: scoped strictly to contest_a (picks contest_a/A.cpp)
  auto latest_ca =
      yrs::find_latest_active_cpp(cfg, tmp.path / "contest_a").value();
  assert(latest_ca.has_value());
  assert(yrs::normalize_path(*latest_ca) == yrs::normalize_path(ca));

  // Dry-run cover-latest does not modify file
  auto dry_cov =
      yrs::cover_latest_cpp(cfg, tmp.path / "contest_a", true).value();
  assert(yrs::normalize_path(dry_cov) == yrs::normalize_path(ca));
  assert(yrs::read_file_to_string(ca).value() == "int ca() {}\n");

  // Real cover-latest inside contest_a covers contest_a/A.cpp, leaves contest_b/M.cpp
  auto real_cov =
      yrs::cover_latest_cpp(cfg, tmp.path / "contest_a", false).value();
  assert(yrs::normalize_path(real_cov) == yrs::normalize_path(ca));
  assert(yrs::read_file_to_string(ca).value() ==
         "// tpl\nint main() { return 0; }\n");
  assert(yrs::read_file_to_string(cb).value() ==
         "#include \"lib.hpp\"\nint cm() { return lv; }\n");

  // Protected directory refuses bundle / cover-latest
  assert(!yrs::bundle_latest_cpp(cfg, tmp.path / "record").has_value());
  assert(!yrs::cover_latest_cpp(cfg, tmp.path / ".wait").has_value());

  if (cli_bin != nullptr) {
    std::string abs_bin = fs::absolute(cli_bin).string();
    fs::path bin_dir = tmp.path / "bin";
    fs::create_directories(bin_dir);
    fs::path wl_copy = bin_dir / "wl-copy";
    write_file(bin_dir, "wl-copy",
               "#!/bin/sh\n/bin/cat > \"$YRS_TEST_CLIPBOARD_CAPTURE\"\n");
    ::chmod(wl_copy.c_str(), 0755);

    fs::path cap = tmp.path / "sub_bundle_cap.txt";
    std::string bcmd =
        "cd \"" + (tmp.path / "contest_b").string() +
        "\" && env -u DISPLAY -u WSL_DISTRO_NAME -u WSL_INTEROP "
        "WAYLAND_DISPLAY=wayland-test PATH=\"" +
        bin_dir.string() + "\" YRS_TEST_CLIPBOARD_CAPTURE=\"" + cap.string() +
        "\" \"" + abs_bin + "\" bundle >/dev/null 2>&1";
    assert(std::system(bcmd.c_str()) == 0);
    assert(yrs::read_file_to_string(cap).value() ==
           "int lv = 11;\nint cm() { return lv; }\n");

    std::string chk_cmd = "cd \"" + (tmp.path / "contest_b").string() +
                          "\" && \"" + abs_bin + "\" check >/dev/null 2>&1";
    assert(std::system(chk_cmd.c_str()) == 0);

    std::string cov_dry =
        "cd \"" + (tmp.path / "contest_b").string() + "\" && \"" + abs_bin +
        "\" cover-latest -n >/dev/null 2>&1";
    assert(std::system(cov_dry.c_str()) == 0);
    assert(yrs::read_file_to_string(cb).value() ==
           "#include \"lib.hpp\"\nint cm() { return lv; }\n");

    std::string stg_dot =
        "cd \"" + (tmp.path / "contest_b").string() + "\" && \"" + abs_bin +
        "\" stage . regional/2026/b >/dev/null 2>&1";
    assert(std::system(stg_dot.c_str()) == 0);
    assert(fs::exists(tmp.path / ".wait" / "regional" / "2026" / "b" /
                      "M.cpp"));
  }
}

void test_clean_unused(const char *cli_bin) {
  TempDir tmp;
  write_default_config(tmp.path);
  std::string tpl_body = "#include \"YRS/all.hpp\"\nvoid solve() {}\nint main() {}\n";
  auto tpl = write_file(tmp.path, "0.cpp", tpl_body);

  std::string tc_unrun =
      "{\"1\":{\"isDisabled\":false,\"result\":null}}";

  // 1. Untouched single problem (with leading URL comment + CRLF)
  auto u_single = write_file(
      tmp.path, "P1001.cpp",
      "// https://www.luogu.com.cn/problem/P1001\r\n"
      "#include \"YRS/all.hpp\"\r\nvoid solve() {}\r\nint main() {}\r\n");
  write_cph_bin(tmp, "P1001.cpp.bin", u_single, "P1001",
                "https://www.luogu.com.cn/problem/P1001", tc_unrun);

  // 2. Empty CPH file
  auto u_empty = write_file(tmp.path, "P1002.cpp", "   \n");
  write_cph_bin(tmp, "P1002.cpp.bin", u_empty, "P1002",
                "https://www.luogu.com.cn/problem/P1002", tc_unrun);

  // 3. Untouched contest problem
  auto u_contest = write_file(tmp.path, "qoj8329.cpp", tpl_body);
  write_cph_bin(tmp, "qoj8329.cpp.bin", u_contest, "B. Empty",
                "https://qoj.ac/contest/1540/problem/8329", tc_unrun);

  // 4. Untouched CPH problem in subdir (subdir should be pruned after removal)
  auto u_sub = write_file(tmp.path / "abandoned_contest", "A.cpp", tpl_body);
  write_cph_bin(tmp, "abandoned_contest/A.cpp.bin", u_sub, "A",
                "https://codeforces.com/contest/2050/problem/A", tc_unrun);

  // Must NOT be touched: modified CPH problem
  auto mod_prob = write_file(tmp.path, "P2000.cpp", "int real_code = 42;\n");
  write_cph_bin(tmp, "P2000.cpp.bin", mod_prob, "P2000",
                "https://www.luogu.com.cn/problem/P2000", tc_unrun);

  // Must NOT be touched: local file without .bin, even with identical template body
  auto local_gen = write_file(tmp.path, "gen.cpp", tpl_body);

  // Must NOT be touched: file inside protected .wait even if a stale .bin points to it
  auto wait_f = write_file(tmp.path / ".wait" / "luogu", "P3000.cpp", tpl_body);
  write_cph_bin(tmp, "wait_ref.cpp.bin", wait_f, "P3000",
                "https://www.luogu.com.cn/problem/P3000", tc_unrun);

  // Orphan .bin
  write_cph_bin(tmp, "ghost.cpp.bin", tmp.path / "ghost.cpp", "Ghost",
                std::nullopt, tc_unrun);

  auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();

  // Default clean (include_unused = false) only removes orphan .bin
  auto only_orphan = yrs::cph::clean_cph_workspace(cfg, false, false).value();
  assert(only_orphan.removed_bins.size() == 1);
  assert(only_orphan.removed_unused.empty());
  assert(!fs::exists(tmp.path / ".cph-ng" / "ghost.cpp.bin"));
  assert(fs::exists(u_single) && fs::exists(u_empty) && fs::exists(u_contest) &&
         fs::exists(u_sub));

  // Add another orphan and test dry-run with include_unused = true
  write_cph_bin(tmp, "ghost2.cpp.bin", tmp.path / "ghost2.cpp", "Ghost2",
                std::nullopt, tc_unrun);
  auto dry_u = yrs::cph::clean_cph_workspace(cfg, true, true).value();
  assert(dry_u.removed_bins.size() == 1);
  assert(dry_u.removed_unused.size() == 4);
  assert(fs::exists(u_single) && fs::exists(u_sub));
  assert(fs::exists(tmp.path / ".cph-ng" / "ghost2.cpp.bin"));

  if (cli_bin != nullptr) {
    std::string abs_bin = fs::absolute(cli_bin).string();
    std::string dry_cli = "cd \"" + tmp.path.string() + "\" && \"" + abs_bin +
                          "\" clean -un >/dev/null 2>&1";
    assert(std::system(dry_cli.c_str()) == 0);
    assert(fs::exists(u_single));

    std::string real_cli = "cd \"" + tmp.path.string() + "\" && \"" + abs_bin +
                           "\" clean -u >/dev/null 2>&1";
    assert(std::system(real_cli.c_str()) == 0);
  } else {
    auto real_u = yrs::cph::clean_cph_workspace(cfg, false, true).value();
    assert(real_u.removed_bins.size() == 1);
    assert(real_u.removed_unused.size() == 4);
  }

  assert(!fs::exists(u_single));
  assert(!fs::exists(tmp.path / ".cph-ng" / "P1001.cpp.bin"));
  assert(!fs::exists(u_empty));
  assert(!fs::exists(u_contest));
  assert(!fs::exists(u_sub));
  assert(!fs::exists(tmp.path / "abandoned_contest"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "abandoned_contest"));
  assert(!fs::exists(tmp.path / ".cph-ng" / "ghost2.cpp.bin"));

  // Immune files must all still exist
  assert(fs::exists(tpl));
  assert(fs::exists(mod_prob));
  assert(fs::exists(tmp.path / ".cph-ng" / "P2000.cpp.bin"));
  assert(fs::exists(local_gen));
  assert(fs::exists(wait_f));
}

void test_workspace_status(const char *cli_bin) {
  TempDir tmp;
  write_default_config(tmp.path);
  std::string tpl_body = "// template\nint main() {}\n";
  set_mtime(write_file(tmp.path, "0.cpp", tpl_body), 1);

  std::string tc_ac2 =
      "{\"1\":{\"isDisabled\":false,\"result\":{\"verdict\":\"AC\"}},"
      "\"2\":{\"isDisabled\":false,\"result\":{\"verdict\":\"AC\"}}}";
  std::string tc_wa =
      "{\"1\":{\"isDisabled\":false,\"result\":{\"verdict\":\"AC\"}},"
      "\"2\":{\"isDisabled\":false,\"result\":{\"verdict\":\"WA\"}}}";
  std::string tc_unrun =
      "{\"1\":{\"isDisabled\":false,\"result\":null}}";

  // Ready to stage
  auto p_ac = write_file(tmp.path, "P2075.cpp", "int ac() { return 0; }\n");
  set_mtime(p_ac, 100);
  write_cph_bin(tmp, "P2075.cpp.bin", p_ac, "P2075",
                "https://www.luogu.com.cn/problem/P2075", tc_ac2);

  // Root contest with P2, P10, P3 (untouched) to verify natural label sort
  auto q10 = write_file(tmp.path, "qoj8337.cpp", "int p10() {}\n");
  set_mtime(q10, 110);
  write_cph_bin(tmp, "qoj8337.cpp.bin", q10, "P10. Ten",
                "https://qoj.ac/contest/1540/problem/8337", tc_ac2);

  auto q2 = write_file(tmp.path, "qoj8329.cpp", "int p2() {}\n");
  set_mtime(q2, 120);
  write_cph_bin(tmp, "qoj8329.cpp.bin", q2, "P2. Two",
                "https://qoj.ac/contest/1540/problem/8329", tc_wa);

  auto q3_untouched = write_file(tmp.path, "qoj8330.cpp", tpl_body);
  set_mtime(q3_untouched, 130);
  write_cph_bin(tmp, "qoj8330.cpp.bin", q3_untouched, "P3. Empty",
                "https://qoj.ac/contest/1540/problem/8330", tc_unrun);

  // Subdir workspace (packed_cf/B.cpp has newest mtime 300)
  auto sub_a = write_file(tmp.path / "packed_cf", "A.cpp", "int sa() {}\n");
  set_mtime(sub_a, 200);
  write_cph_bin(tmp, "packed_cf/A.cpp.bin", sub_a, "A",
                "https://codeforces.com/contest/2050/problem/A", tc_ac2);

  auto sub_b = write_file(tmp.path / "packed_cf", "B.cpp", "int sb() {}\n");
  set_mtime(sub_b, 300);
  write_cph_bin(tmp, "packed_cf/B.cpp.bin", sub_b, "B",
                "https://codeforces.com/contest/2050/problem/B", tc_unrun);

  // In progress singles: 1 WA + 1 standalone unrouted AC
  auto p_wa = write_file(tmp.path, "P9999.cpp", "int wa() {}\n");
  set_mtime(p_wa, 140);
  write_cph_bin(tmp, "P9999.cpp.bin", p_wa, "P9999",
                "https://www.luogu.com.cn/problem/P9999", tc_wa);

  auto q_single_ac = write_file(tmp.path, "qoj9000.cpp", "int q9() {}\n");
  set_mtime(q_single_ac, 150);
  write_cph_bin(tmp, "qoj9000.cpp.bin", q_single_ac, "Standalone",
                "https://qoj.ac/problem/9000", tc_ac2);

  // Untouched single template
  auto p_untouched = write_file(tmp.path, "P8000.cpp", tpl_body);
  set_mtime(p_untouched, 160);
  write_cph_bin(tmp, "P8000.cpp.bin", p_untouched, "P8000",
                "https://www.luogu.com.cn/problem/P8000", tc_unrun);

  // Local non-CPH file
  set_mtime(write_file(tmp.path, "gen.cpp", "int gen() {}\n"), 170);

  // Catalog (.wait) files + Orphan .bin
  write_file(tmp.path / ".wait" / "luogu", "P1000.cpp", "int x() {}\n");
  write_file(tmp.path / ".wait" / "regional" / "cn", "X.cpp", "int y() {}\n");
  write_cph_bin(tmp, "dead.cpp.bin", tmp.path / "dead.cpp", "Dead",
                std::nullopt, tc_unrun);

  auto cfg = yrs::AppConfig::load_for_dir(tmp.path).value();
  auto st = yrs::cph::inspect_workspace_status(cfg, tmp.path).value();

  assert(st.latest_active_cpp.has_value());
  assert(yrs::normalize_path(*st.latest_active_cpp) ==
         yrs::normalize_path(sub_b));
  assert(st.ready_to_stage.size() == 1);
  assert(st.ready_to_stage[0].src_path.filename() == "P2075.cpp");

  assert(st.root_contests.size() == 1);
  assert(st.root_contests[0].info.key == "qoj:1540");
  assert(st.root_contests[0].ac_count == 1);
  assert(st.root_contests[0].wip_count == 1);
  assert(st.root_contests[0].untouched_count == 1);
  assert(st.root_contests[0].items.size() == 3);
  // Natural sort: P2 < P3 < P10
  assert(st.root_contests[0].items[0].label == "P2");
  assert(st.root_contests[0].items[1].label == "P3");
  assert(st.root_contests[0].items[2].label == "P10");

  assert(st.subdir_workspaces.size() == 1);
  assert(st.subdir_workspaces[0].rel_name == "packed_cf");
  assert(st.subdir_workspaces[0].ac_count == 1);
  assert(st.subdir_workspaces[0].wip_count == 1);

  assert(st.in_progress_singles.size() == 2);
  assert(st.untouched_singles.size() == 1);
  assert(st.untouched_singles[0].src_path.filename() == "P8000.cpp");
  assert(st.local_root_files.size() == 1);
  assert(st.local_root_files[0].filename() == "gen.cpp");
  assert(st.catalog_file_count == 2);
  assert(st.orphan_bin_count == 1);
  assert(st.total_unused_cph_count == 2);

  std::string compact_out = yrs::cph::render_workspace_status(st, false);
  assert(compact_out.find("packed_cf/B.cpp") != std::string::npos);
  assert(compact_out.find("+ 1 untouched template(s)") != std::string::npos);
  assert(compact_out.find("qoj8330.cpp") == std::string::npos);
  assert(compact_out.find("WA 1/2") != std::string::npos);
  assert(compact_out.find("AC 2/2, manual") != std::string::npos);

  std::string verbose_out = yrs::cph::render_workspace_status(st, true);
  assert(verbose_out.find("qoj8330.cpp -> P3.cpp") != std::string::npos);

  if (cli_bin != nullptr) {
    std::string abs_bin = fs::absolute(cli_bin).string();
    fs::path out_file = tmp.path / "status_cli.txt";
    std::string scmd = "cd \"" + (tmp.path / "packed_cf").string() +
                       "\" && \"" + abs_bin + "\" status -v > \"" +
                       out_file.string() + "\" 2>&1";
    assert(std::system(scmd.c_str()) == 0);
    std::string cli_rendered = yrs::read_file_to_string(out_file).value();
    assert(cli_rendered.find("cwd") != std::string::npos);
    assert(cli_rendered.find("qoj8330.cpp -> P3.cpp") != std::string::npos);
  }
}

}  // namespace

int main(int argc, char **argv) {
  test_config_load();
  test_config_discover();
  test_latest_and_cover();
  test_subdir_latest_and_cover(argc > 1 ? argv[1] : nullptr);
  test_bundle();
  test_move_and_history();
  test_paths_and_symlinks();
  test_atomic_write_and_move();
  test_sigpipe_safety();
  test_gzip_and_json();
  test_cph_stage_and_clean(argc > 1 ? argv[1] : nullptr);
  test_clean_unused(argc > 1 ? argv[1] : nullptr);
  test_cph_pack(argc > 1 ? argv[1] : nullptr);
  test_workspace_status(argc > 1 ? argv[1] : nullptr);
  if (argc > 1) {
    test_cli_integration(argv[1]);
  }
  std::printf("All tests passed!\n");
  return 0;
}

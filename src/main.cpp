#include "yrs.hpp"

#include <csignal>
#include <cstdio>

namespace {

constexpr const char *ANSI_RED = "\x1b[31m";
constexpr const char *ANSI_RESET = "\x1b[0m";

void print_help(const char *prog) {
  std::printf(
      "Usage: %s <COMMAND> [ARGS...]\n\n"
      "Commands:\n"
      "  bundle        Expand local #include \"...\" in latest (or given) .cpp "
      "and copy to clipboard\n"
      "                  Usage: bundle [<FILE>]\n"
      "  check         Inspect yrs.toml configuration in current directory\n"
      "  clean         Remove orphan .cph-ng/*.bin cache files\n"
      "                  Options: [--dry-run | -n]\n"
      "  cover-latest  Overwrite latest .cpp with template source\n"
      "  move          Move files from catalog_root into record_root and "
      "update summary\n"
      "  pack          Pack root contest .cpp files into <CONTEST_DIR>/ "
      "(rename to A.cpp.., drop untouched templates, relocate .bin)\n"
      "                  Usage: pack | pack [--dry-run | -n] [<CONTEST_ID>] "
      "<CONTEST_DIR>\n"
      "  stage         Stage AC single-problem .cpp (or a contest dir) into "
      "catalog_root\n"
      "                  Usage: stage [--dry-run | -n] [<SRC_DIR> "
      "<WAIT_SUBDIR>]\n"
      "  help          Print this message\n",
      prog);
}

void print_check_row(std::string_view label, const std::string &val) {
  constexpr size_t W = 15;
  size_t len = label.size();
  size_t pad = (len < W) ? (W - len) : 0;
  size_t left = pad / 2;
  size_t right = pad - left;
  std::printf("%*s%.*s%*s %s\n", static_cast<int>(left), "",
              static_cast<int>(len), label.data(), static_cast<int>(right), "",
              val.c_str());
}

yrs::Result<void> run_check() {
  std::error_code ec;
  auto cwd = yrs::fs::current_path(ec);
  if (ec) return std::unexpected(yrs::make_err(yrs::ErrKind::Other, ec.message()));

  auto cfg_res = yrs::AppConfig::load_for_dir(cwd);
  if (!cfg_res) return std::unexpected(cfg_res.error());
  const auto &cfg = *cfg_res;

  print_check_row("cwd", cwd.string());
  print_check_row("workspace root", cfg.workspace_root.string());
  print_check_row("library root", cfg.library_root.string());
  print_check_row("catalog root", cfg.catalog_root.string());
  print_check_row("record root", cfg.record_root.string());
  print_check_row("summary file", cfg.summary_path.string());
  print_check_row("template src", cfg.template_source.string());
  return {};
}

yrs::Result<void> run_cover_latest() {
  std::error_code ec;
  auto cwd = yrs::fs::current_path(ec);
  if (ec) return std::unexpected(yrs::make_err(yrs::ErrKind::Other, ec.message()));

  auto cfg_res = yrs::AppConfig::load_for_dir(cwd);
  if (!cfg_res) return std::unexpected(cfg_res.error());

  auto target_res = yrs::cover_latest_cpp(*cfg_res);
  if (!target_res) return std::unexpected(target_res.error());

  std::printf("covered %s\n", target_res->c_str());
  return {};
}

yrs::Result<void> run_bundle(std::optional<std::string_view> file_arg) {
  std::error_code ec;
  auto cwd = yrs::fs::current_path(ec);
  if (ec) return std::unexpected(yrs::make_err(yrs::ErrKind::Other, ec.message()));

  auto cfg_res = yrs::AppConfig::load_for_dir(cwd);
  if (!cfg_res) return std::unexpected(cfg_res.error());

  yrs::Result<yrs::BundledLatest> bundled_res =
      file_arg
          ? [&]() -> yrs::Result<yrs::BundledLatest> {
              yrs::fs::path p(*file_arg);
              if (!p.is_absolute()) p = cwd / p;
              if (!yrs::fs::is_regular_file(p, ec)) {
                return std::unexpected(yrs::make_err(
                    yrs::ErrKind::NotFound, "missing cpp file: " + p.string()));
              }
              return yrs::bundle_cpp_path(*cfg_res, p);
            }()
          : yrs::bundle_latest_cpp(*cfg_res);
  if (!bundled_res) return std::unexpected(bundled_res.error());

  auto clip_res = yrs::clipboard::copy_to_clipboard(bundled_res->content);
  if (!clip_res) return std::unexpected(clip_res.error());

  std::printf("bundled %s to clipboard\n", bundled_res->source_path.c_str());
  return {};
}

yrs::Result<void> run_move() {
  std::error_code ec;
  auto cwd = yrs::fs::current_path(ec);
  if (ec) return std::unexpected(yrs::make_err(yrs::ErrKind::Other, ec.message()));

  auto cfg_res = yrs::AppConfig::load_for_dir(cwd);
  if (!cfg_res) return std::unexpected(cfg_res.error());
  const auto &cfg = *cfg_res;

  auto move_res = yrs::move_catalog_into_record(cfg);
  if (!move_res) return std::unexpected(move_res.error());

  auto day_groups = yrs::build_move_day_groups(move_res->entries);
  std::string terminal_output =
      yrs::render::render_grouped_move_trees(cfg.record_root, day_groups);

  auto sum_res = yrs::update_summary_file(cfg, day_groups);
  if (!sum_res) return std::unexpected(sum_res.error());

  auto clean_res = yrs::cph::clean_orphan_cph_bins(cfg, false);
  if (!clean_res) return std::unexpected(clean_res.error());

  std::printf("moved %zu files into %s\n\n%s\n", move_res->entries.size(),
              cfg.record_root.c_str(), terminal_output.c_str());
  if (!clean_res->removed_bins.empty()) {
    std::printf("\ncleaned %zu orphan .cph-ng cache files\n",
                clean_res->removed_bins.size());
  }
  return {};
}

yrs::Result<void> run_stage(const std::vector<std::string_view> &pos_args,
                            bool dry_run) {
  std::error_code ec;
  auto cwd = yrs::fs::current_path(ec);
  if (ec) return std::unexpected(yrs::make_err(yrs::ErrKind::Other, ec.message()));

  auto cfg_res = yrs::AppConfig::load_for_dir(cwd);
  if (!cfg_res) return std::unexpected(cfg_res.error());
  const auto &cfg = *cfg_res;

  auto stage_res =
      pos_args.empty()
          ? yrs::cph::stage_single_problems(cfg, dry_run)
          : yrs::cph::stage_directory(cfg, pos_args[0], pos_args[1], dry_run);
  if (!stage_res) return std::unexpected(stage_res.error());

  const char *prefix = dry_run ? "[dry-run] " : "";
  if (stage_res->entries.empty()) {
    std::printf("%sno files to stage\n", prefix);
    return {};
  }

  std::printf("%s%s %zu files:\n", prefix,
              dry_run ? "would stage" : "staged", stage_res->entries.size());
  for (const auto &e : stage_res->entries) {
    auto rel_src = e.src_path.lexically_relative(cfg.workspace_root);
    std::string src_str =
        rel_src.empty() ? e.src_path.string() : rel_src.string();
    auto rel_dst = e.dst_path.lexically_relative(cfg.workspace_root);
    std::string dst_str =
        rel_dst.empty() ? e.dst_path.string() : rel_dst.string();
    std::printf("  %s -> %s%s\n", src_str.c_str(), dst_str.c_str(),
                e.bin_removed ? (dry_run ? " (would drop .bin)" : " (-bin)")
                              : "");
  }
  return {};
}

yrs::Result<void> run_clean(bool dry_run) {
  std::error_code ec;
  auto cwd = yrs::fs::current_path(ec);
  if (ec) return std::unexpected(yrs::make_err(yrs::ErrKind::Other, ec.message()));

  auto cfg_res = yrs::AppConfig::load_for_dir(cwd);
  if (!cfg_res) return std::unexpected(cfg_res.error());
  const auto &cfg = *cfg_res;

  auto clean_res = yrs::cph::clean_orphan_cph_bins(cfg, dry_run);
  if (!clean_res) return std::unexpected(clean_res.error());

  const char *prefix = dry_run ? "[dry-run] " : "";
  if (clean_res->removed_bins.empty()) {
    std::printf("%sno orphan .cph-ng cache files\n", prefix);
    return {};
  }

  std::printf("%s%s %zu orphan .cph-ng cache files:\n", prefix,
              dry_run ? "would clean" : "cleaned",
              clean_res->removed_bins.size());
  for (const auto &b : clean_res->removed_bins) {
    auto rel = b.lexically_relative(cfg.workspace_root);
    std::string s = rel.empty() ? b.string() : rel.string();
    std::printf("  %s\n", s.c_str());
  }
  return {};
}

yrs::Result<void> run_pack(const std::vector<std::string_view> &pos_args,
                           bool dry_run) {
  std::error_code ec;
  auto cwd = yrs::fs::current_path(ec);
  if (ec) return std::unexpected(yrs::make_err(yrs::ErrKind::Other, ec.message()));

  auto cfg_res = yrs::AppConfig::load_for_dir(cwd);
  if (!cfg_res) return std::unexpected(cfg_res.error());
  const auto &cfg = *cfg_res;

  if (pos_args.empty()) {
    auto groups_res = yrs::cph::list_root_contests(cfg);
    if (!groups_res) return std::unexpected(groups_res.error());
    if (groups_res->empty()) {
      std::printf("no contest problems found in workspace root\n");
      return {};
    }
    for (const auto &g : *groups_res) {
      std::printf("%s  %zu files (%zu modified, %zu template):\n",
                  g.info.key.c_str(), g.items.size(), g.modified_count,
                  g.untouched_count);
      for (const auto &item : g.items) {
        std::string src_name = item.src_path.filename().string();
        if (item.untouched) {
          std::printf("  %s (untouched template -> drop)\n", src_name.c_str());
        } else {
          std::printf("  %s -> %s.cpp\n", src_name.c_str(),
                      item.label.c_str());
        }
      }
    }
    return {};
  }

  std::optional<std::string_view> selector;
  yrs::fs::path dst_dir;
  if (pos_args.size() == 1) {
    dst_dir = yrs::fs::path(pos_args[0]);
  } else {
    selector = pos_args[0];
    dst_dir = yrs::fs::path(pos_args[1]);
  }

  auto pack_res = yrs::cph::pack_contest(cfg, selector, dst_dir, dry_run);
  if (!pack_res) return std::unexpected(pack_res.error());

  const char *prefix = dry_run ? "[dry-run] " : "";
  auto rel_dir = pack_res->dst_dir.lexically_relative(cfg.workspace_root);
  std::string dir_str =
      rel_dir.empty() ? pack_res->dst_dir.string() : rel_dir.string();

  std::printf("%s%s %s (%zu packed, %zu dropped templates):\n", prefix,
              dry_run ? "would pack" : "packed", pack_res->info.key.c_str(),
              pack_res->packed_files.size(),
              pack_res->dropped_templates.size());
  for (const auto &e : pack_res->packed_files) {
    auto rel_dst = e.dst_path.lexically_relative(cfg.workspace_root);
    std::string dst_str =
        rel_dst.empty() ? e.dst_path.string() : rel_dst.string();
    std::printf("  %s -> %s (relocated .bin)\n",
                e.src_path.filename().c_str(), dst_str.c_str());
  }
  for (const auto &d : pack_res->dropped_templates) {
    std::printf("  %s (%s)\n", d.filename().c_str(),
                dry_run ? "would drop template + .bin" : "dropped template");
  }
  (void)dir_str;
  return {};
}

}  // namespace

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);

  if (argc < 2) {
    print_help(argc > 0 ? argv[0] : "yrs-cli");
    return 2;
  }

  std::string_view cmd = argv[1];
  if (cmd == "-h" || cmd == "--help" || cmd == "help") {
    print_help(argv[0]);
    return 0;
  }

  if (cmd != "bundle" && cmd != "check" && cmd != "cover-latest" &&
      cmd != "move" && cmd != "pack" && cmd != "stage" && cmd != "clean") {
    std::fprintf(stderr, "%serror%s: unrecognized subcommand `%s`\n", ANSI_RED,
                 ANSI_RESET, argv[1]);
    print_help(argv[0]);
    return 2;
  }

  bool dry_run = false;
  std::vector<std::string_view> pos_args;

  if (cmd == "stage" || cmd == "clean" || cmd == "pack" || cmd == "bundle") {
    for (int i = 2; i < argc; ++i) {
      std::string_view arg = argv[i];
      if ((cmd == "stage" || cmd == "clean" || cmd == "pack") &&
          (arg == "--dry-run" || arg == "-n")) {
        dry_run = true;
      } else if (arg == "-h" || arg == "--help") {
        print_help(argv[0]);
        return 0;
      } else if (arg.starts_with("-")) {
        std::fprintf(stderr, "%serror%s: unexpected flag `%s`\n", ANSI_RED,
                     ANSI_RESET, argv[i]);
        return 2;
      } else {
        pos_args.push_back(arg);
      }
    }
    if (cmd == "clean" && !pos_args.empty()) {
      std::fprintf(stderr, "%serror%s: unexpected argument `%.*s`\n", ANSI_RED,
                   ANSI_RESET, static_cast<int>(pos_args[0].size()),
                   pos_args[0].data());
      return 2;
    }
    if (cmd == "bundle" && pos_args.size() > 1) {
      std::fprintf(stderr, "%serror%s: `bundle` accepts at most 1 file path\n",
                   ANSI_RED, ANSI_RESET);
      return 2;
    }
    if (cmd == "pack" && pos_args.size() > 2) {
      std::fprintf(stderr,
                   "%serror%s: `pack` expects 0, 1 (<CONTEST_DIR>), or 2 "
                   "(<CONTEST_ID> <CONTEST_DIR>) arguments\n",
                   ANSI_RED, ANSI_RESET);
      return 2;
    }
    if (cmd == "stage" && !pos_args.empty() && pos_args.size() != 2) {
      std::fprintf(stderr,
                   "%serror%s: `stage` expects either 0 arguments or "
                   "<SRC_DIR> <WAIT_SUBDIR>\n",
                   ANSI_RED, ANSI_RESET);
      return 2;
    }
  } else if (argc > 2) {
    std::fprintf(stderr, "%serror%s: unexpected argument `%s`\n", ANSI_RED,
                 ANSI_RESET, argv[2]);
    return 2;
  }

  std::printf("\n%.*s\n\n", static_cast<int>(yrs::ciallo().size()),
              yrs::ciallo().data());

  if (cmd == "bundle") {
    std::optional<std::string_view> file_arg;
    if (!pos_args.empty()) file_arg = pos_args[0];
    if (auto r = run_bundle(file_arg); !r) {
      std::fprintf(stderr, "%sbundle failed%s: %s\n", ANSI_RED, ANSI_RESET,
                   r.error().message.c_str());
      return 1;
    }
  } else if (cmd == "check") {
    if (auto r = run_check(); !r) {
      std::fprintf(stderr, "%scheck failed%s: %s\n", ANSI_RED, ANSI_RESET,
                   r.error().message.c_str());
      return 1;
    }
  } else if (cmd == "cover-latest") {
    if (auto r = run_cover_latest(); !r) {
      std::fprintf(stderr, "%scover failed%s: %s\n", ANSI_RED, ANSI_RESET,
                   r.error().message.c_str());
      return 1;
    }
  } else if (cmd == "move") {
    if (auto r = run_move(); !r) {
      std::fprintf(stderr, "%smove failed%s: %s\n", ANSI_RED, ANSI_RESET,
                   r.error().message.c_str());
      return 1;
    }
  } else if (cmd == "pack") {
    if (auto r = run_pack(pos_args, dry_run); !r) {
      std::fprintf(stderr, "%spack failed%s: %s\n", ANSI_RED, ANSI_RESET,
                   r.error().message.c_str());
      return 1;
    }
  } else if (cmd == "stage") {
    if (auto r = run_stage(pos_args, dry_run); !r) {
      std::fprintf(stderr, "%sstage failed%s: %s\n", ANSI_RED, ANSI_RESET,
                   r.error().message.c_str());
      return 1;
    }
  } else if (cmd == "clean") {
    if (auto r = run_clean(dry_run); !r) {
      std::fprintf(stderr, "%sclean failed%s: %s\n", ANSI_RED, ANSI_RESET,
                   r.error().message.c_str());
      return 1;
    }
  }

  return 0;
}

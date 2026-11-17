#include "yrs.hpp"

#include <array>
#include <csignal>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_red_err_prefix(std::string_view label) {
  std::fprintf(stderr, "%.*s%.*s%.*s",
               static_cast<int>(yrs::term::RED.size()), yrs::term::RED.data(),
               static_cast<int>(label.size()), label.data(),
               static_cast<int>(yrs::term::RESET.size()),
               yrs::term::RESET.data());
}

void print_help(const char *prog) {
  std::printf(
      "Usage: %s <COMMAND> [ARGS...]\n\n"
      "Commands:\n"
      "  bundle        Expand local #include \"...\" in latest (or given) .cpp "
      "and copy to clipboard\n"
      "                  Usage: bundle [<FILE>]\n"
      "  check         Inspect yrs.toml configuration (searches current and "
      "parent directories)\n"
      "  clean         Remove orphan .cph-ng/*.bin cache files (and untouched "
      "CPH templates with -u)\n"
      "                  Options: [--dry-run | -n] [--unused | -u]\n"
      "  cover-latest  Overwrite latest active .cpp with template source\n"
      "                  Options: [--dry-run | -n]\n"
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
      "  status        Show workspace dashboard (active files, contests, "
      "staging queue, cache)\n"
      "                  Options: [--verbose | -v]\n"
      "  help          Print this message\n",
      prog);
}

std::string ws_rel_str(const yrs::AppConfig &cfg, const yrs::fs::path &p) {
  return yrs::rel_display(yrs::normalize_path(cfg.workspace_root), p);
}

struct CliContext {
  bool dry_run = false;
  bool include_unused = false;
  bool verbose = false;
  std::vector<std::string_view> pos_args;
};

struct CommandContext {
  yrs::fs::path cwd;
  yrs::AppConfig cfg;
};

yrs::Result<CommandContext> load_command_context() {
  std::error_code ec;
  auto cwd = yrs::fs::current_path(ec);
  if (ec) {
    return std::unexpected(yrs::make_err(yrs::ErrKind::Other, ec.message()));
  }
  auto cfg_res = yrs::AppConfig::discover_for_dir(cwd);
  if (!cfg_res) return std::unexpected(cfg_res.error());
  return CommandContext{std::move(cwd), std::move(*cfg_res)};
}

yrs::Result<void> run_check(const CliContext &) {
  auto ctx_res = load_command_context();
  if (!ctx_res) return std::unexpected(ctx_res.error());
  const auto &cfg = ctx_res->cfg;

  yrs::term::print_kv_row("cwd", ctx_res->cwd.string());
  yrs::term::print_kv_row("workspace root", cfg.workspace_root.string());
  yrs::term::print_kv_row("library root", cfg.library_root.string());
  yrs::term::print_kv_row("catalog root", cfg.catalog_root.string());
  yrs::term::print_kv_row("record root", cfg.record_root.string());
  yrs::term::print_kv_row("summary file", cfg.summary_path.string());
  yrs::term::print_kv_row("template src", cfg.template_source.string());
  return {};
}

yrs::Result<void> run_cover_latest(const CliContext &cli) {
  auto ctx_res = load_command_context();
  if (!ctx_res) return std::unexpected(ctx_res.error());

  auto target_res =
      yrs::cover_latest_cpp(ctx_res->cfg, ctx_res->cwd, cli.dry_run);
  if (!target_res) return std::unexpected(target_res.error());

  if (cli.dry_run) {
    std::printf("[dry-run] would cover %s\n", target_res->c_str());
  } else {
    std::printf("covered %s\n", target_res->c_str());
  }
  return {};
}

yrs::Result<void> run_bundle(const CliContext &cli) {
  auto ctx_res = load_command_context();
  if (!ctx_res) return std::unexpected(ctx_res.error());

  std::optional<std::string_view> file_arg;
  if (!cli.pos_args.empty()) file_arg = cli.pos_args[0];

  yrs::Result<yrs::BundledLatest> bundled_res =
      file_arg
          ? [&]() -> yrs::Result<yrs::BundledLatest> {
              std::error_code ec;
              yrs::fs::path p(*file_arg);
              if (!p.is_absolute()) p = ctx_res->cwd / p;
              if (!yrs::fs::is_regular_file(p, ec)) {
                return std::unexpected(yrs::make_err(
                    yrs::ErrKind::NotFound, "missing cpp file: " + p.string()));
              }
              return yrs::bundle_cpp_path(ctx_res->cfg, p);
            }()
          : yrs::bundle_latest_cpp(ctx_res->cfg, ctx_res->cwd);
  if (!bundled_res) return std::unexpected(bundled_res.error());

  auto clip_res = yrs::clipboard::copy_to_clipboard(bundled_res->content);
  if (!clip_res) return std::unexpected(clip_res.error());

  std::printf("bundled %s to clipboard\n", bundled_res->source_path.c_str());
  return {};
}

yrs::Result<void> run_move(const CliContext &) {
  auto ctx_res = load_command_context();
  if (!ctx_res) return std::unexpected(ctx_res.error());
  const auto &cfg = ctx_res->cfg;

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

yrs::Result<void> run_stage(const CliContext &cli) {
  auto ctx_res = load_command_context();
  if (!ctx_res) return std::unexpected(ctx_res.error());
  const auto &cfg = ctx_res->cfg;

  auto stage_res = [&]() -> yrs::Result<yrs::cph::StageSummary> {
    if (cli.pos_args.empty()) {
      return yrs::cph::stage_single_problems(cfg, cli.dry_run);
    }
    std::error_code ec;
    yrs::fs::path src_arg(cli.pos_args[0]);
    if (!src_arg.is_absolute() &&
        yrs::normalize_path(ctx_res->cwd) !=
            yrs::normalize_path(cfg.workspace_root)) {
      yrs::fs::path from_cwd = ctx_res->cwd / src_arg;
      if (yrs::fs::exists(from_cwd, ec)) {
        src_arg = std::move(from_cwd);
      }
      ec.clear();
    }
    return yrs::cph::stage_directory(cfg, src_arg, cli.pos_args[1],
                                     cli.dry_run);
  }();
  if (!stage_res) return std::unexpected(stage_res.error());

  const char *prefix = cli.dry_run ? "[dry-run] " : "";
  if (stage_res->entries.empty()) {
    std::printf("%sno files to stage\n", prefix);
    return {};
  }

  std::printf("%s%s %zu files:\n", prefix,
              cli.dry_run ? "would stage" : "staged",
              stage_res->entries.size());
  for (const auto &e : stage_res->entries) {
    std::string src_str = ws_rel_str(cfg, e.src_path);
    std::string dst_str = ws_rel_str(cfg, e.dst_path);
    std::printf("  %s -> %s%s\n", src_str.c_str(), dst_str.c_str(),
                e.bin_removed ? (cli.dry_run ? " (would drop .bin)" : " (-bin)")
                              : "");
  }
  return {};
}

yrs::Result<void> run_clean(const CliContext &cli) {
  auto ctx_res = load_command_context();
  if (!ctx_res) return std::unexpected(ctx_res.error());
  const auto &cfg = ctx_res->cfg;

  auto clean_res =
      yrs::cph::clean_cph_workspace(cfg, cli.dry_run, cli.include_unused);
  if (!clean_res) return std::unexpected(clean_res.error());

  const char *prefix = cli.dry_run ? "[dry-run] " : "";
  if (clean_res->removed_bins.empty() && clean_res->removed_unused.empty()) {
    if (cli.include_unused) {
      std::printf("%sno orphan .cph-ng cache files or unused templates\n",
                  prefix);
    } else {
      std::printf("%sno orphan .cph-ng cache files\n", prefix);
    }
    return {};
  }

  if (!clean_res->removed_unused.empty()) {
    std::printf("%s%s %zu unused template problem(s):\n", prefix,
                cli.dry_run ? "would clean" : "cleaned",
                clean_res->removed_unused.size());
    for (const auto &u : clean_res->removed_unused) {
      std::string s = ws_rel_str(cfg, u.src_path);
      std::printf("  %s (%s)\n", s.c_str(),
                  cli.dry_run ? "would drop .cpp + .bin"
                              : "dropped .cpp + .bin");
    }
  }

  if (!clean_res->removed_bins.empty()) {
    if (!clean_res->removed_unused.empty()) std::printf("\n");
    std::printf("%s%s %zu orphan .cph-ng cache files:\n", prefix,
                cli.dry_run ? "would clean" : "cleaned",
                clean_res->removed_bins.size());
    for (const auto &b : clean_res->removed_bins) {
      std::string s = ws_rel_str(cfg, b);
      std::printf("  %s\n", s.c_str());
    }
  }
  return {};
}

yrs::Result<void> run_pack(const CliContext &cli) {
  auto ctx_res = load_command_context();
  if (!ctx_res) return std::unexpected(ctx_res.error());
  const auto &cfg = ctx_res->cfg;

  if (cli.pos_args.empty()) {
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
  if (cli.pos_args.size() == 1) {
    dst_dir = yrs::fs::path(cli.pos_args[0]);
  } else {
    selector = cli.pos_args[0];
    dst_dir = yrs::fs::path(cli.pos_args[1]);
  }

  auto pack_res =
      yrs::cph::pack_contest(cfg, selector, dst_dir, cli.dry_run);
  if (!pack_res) return std::unexpected(pack_res.error());

  const char *prefix = cli.dry_run ? "[dry-run] " : "";
  std::printf("%s%s %s (%zu packed, %zu dropped templates):\n", prefix,
              cli.dry_run ? "would pack" : "packed",
              pack_res->info.key.c_str(), pack_res->packed_files.size(),
              pack_res->dropped_templates.size());
  for (const auto &e : pack_res->packed_files) {
    std::string dst_str = ws_rel_str(cfg, e.dst_path);
    std::printf("  %s -> %s (relocated .bin)\n",
                e.src_path.filename().c_str(), dst_str.c_str());
  }
  for (const auto &d : pack_res->dropped_templates) {
    std::printf("  %s (%s)\n", d.filename().c_str(),
                cli.dry_run ? "would drop template + .bin"
                            : "dropped template");
  }
  return {};
}

yrs::Result<void> run_status(const CliContext &cli) {
  auto ctx_res = load_command_context();
  if (!ctx_res) return std::unexpected(ctx_res.error());

  auto st_res = yrs::cph::inspect_workspace_status(ctx_res->cfg, ctx_res->cwd);
  if (!st_res) return std::unexpected(st_res.error());

  std::string rendered =
      yrs::cph::render_workspace_status(*st_res, cli.verbose);
  std::printf("%s\n", rendered.c_str());
  return {};
}

enum class PosArgRule {
  None,
  ZeroOrOne,
  ZeroToTwo,
  ZeroOrTwo,
};

struct SubcommandSpec {
  std::string_view name;
  std::string_view fail_label;
  bool allow_dry_run;
  bool allow_unused;
  bool allow_verbose;
  PosArgRule pos_rule;
  yrs::Result<void> (*handler)(const CliContext &);
};

constexpr std::array<SubcommandSpec, 8> SUBCOMMANDS = {{
    {"bundle", "bundle failed", false, false, false, PosArgRule::ZeroOrOne,
     run_bundle},
    {"check", "check failed", false, false, false, PosArgRule::None, run_check},
    {"cover-latest", "cover failed", true, false, false, PosArgRule::None,
     run_cover_latest},
    {"move", "move failed", false, false, false, PosArgRule::None, run_move},
    {"pack", "pack failed", true, false, false, PosArgRule::ZeroToTwo,
     run_pack},
    {"stage", "stage failed", true, false, false, PosArgRule::ZeroOrTwo,
     run_stage},
    {"clean", "clean failed", true, true, false, PosArgRule::None, run_clean},
    {"status", "status failed", false, false, true, PosArgRule::None,
     run_status},
}};

const SubcommandSpec *find_subcommand(std::string_view cmd) {
  for (const auto &spec : SUBCOMMANDS) {
    if (spec.name == cmd) return &spec;
  }
  return nullptr;
}

bool validate_pos_args(const SubcommandSpec &spec,
                       const std::vector<std::string_view> &pos_args) {
  switch (spec.pos_rule) {
    case PosArgRule::None:
      if (!pos_args.empty()) {
        print_red_err_prefix("error");
        std::fprintf(stderr, ": unexpected argument `%.*s`\n",
                     static_cast<int>(pos_args[0].size()), pos_args[0].data());
        return false;
      }
      return true;
    case PosArgRule::ZeroOrOne:
      if (pos_args.size() > 1) {
        print_red_err_prefix("error");
        std::fprintf(stderr, ": `bundle` accepts at most 1 file path\n");
        return false;
      }
      return true;
    case PosArgRule::ZeroToTwo:
      if (pos_args.size() > 2) {
        print_red_err_prefix("error");
        std::fprintf(stderr,
                     ": `pack` expects 0, 1 (<CONTEST_DIR>), or 2 "
                     "(<CONTEST_ID> <CONTEST_DIR>) arguments\n");
        return false;
      }
      return true;
    case PosArgRule::ZeroOrTwo:
      if (!pos_args.empty() && pos_args.size() != 2) {
        print_red_err_prefix("error");
        std::fprintf(stderr,
                     ": `stage` expects either 0 arguments or "
                     "<SRC_DIR> <WAIT_SUBDIR>\n");
        return false;
      }
      return true;
  }
  return false;
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

  const SubcommandSpec *spec = find_subcommand(cmd);
  if (!spec) {
    print_red_err_prefix("error");
    std::fprintf(stderr, ": unrecognized subcommand `%s`\n", argv[1]);
    print_help(argv[0]);
    return 2;
  }

  CliContext cli;
  for (int i = 2; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help(argv[0]);
      return 0;
    }
    if (arg == "--dry-run" && spec->allow_dry_run) {
      cli.dry_run = true;
      continue;
    }
    if (arg == "--unused" && spec->allow_unused) {
      cli.include_unused = true;
      continue;
    }
    if (arg == "--verbose" && spec->allow_verbose) {
      cli.verbose = true;
      continue;
    }
    if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
      bool ok = true;
      for (size_t k = 1; k < arg.size(); ++k) {
        char f = arg[k];
        if (f == 'n' && spec->allow_dry_run) {
          cli.dry_run = true;
        } else if (f == 'u' && spec->allow_unused) {
          cli.include_unused = true;
        } else if (f == 'v' && spec->allow_verbose) {
          cli.verbose = true;
        } else {
          ok = false;
          break;
        }
      }
      if (ok) continue;
    }
    if (arg.starts_with("-")) {
      print_red_err_prefix("error");
      std::fprintf(stderr, ": unexpected flag `%s`\n", argv[i]);
      return 2;
    }
    cli.pos_args.push_back(arg);
  }

  if (!validate_pos_args(*spec, cli.pos_args)) {
    return 2;
  }

  std::printf("\n%.*s\n\n", static_cast<int>(yrs::ciallo().size()),
              yrs::ciallo().data());

  if (auto r = spec->handler(cli); !r) {
    print_red_err_prefix(spec->fail_label);
    std::fprintf(stderr, ": %s\n", r.error().message.c_str());
    return 1;
  }
  return 0;
}

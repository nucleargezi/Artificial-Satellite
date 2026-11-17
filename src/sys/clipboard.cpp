#include "sys/clipboard.hpp"

#include "core/fs_util.hpp"
#include "core/str_util.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace yrs::clipboard {

namespace {

bool has_env(const char *name) {
  const char *val = std::getenv(name);
  return val != nullptr && val[0] != '\0';
}

}  // namespace

Result<void> run_clipboard_command(const char *program,
                                   const std::vector<const char *> &args,
                                   std::string_view content) {
  int stdin_pipe[2];
  if (::pipe(stdin_pipe) != 0) return std::unexpected(from_errno("pipe"));

  int err_pipe[2];
  if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
    ::close(stdin_pipe[0]);
    ::close(stdin_pipe[1]);
    return std::unexpected(from_errno("pipe2"));
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(stdin_pipe[0]);
    ::close(stdin_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    return std::unexpected(from_errno("fork"));
  }

  if (pid == 0) {
    ::close(stdin_pipe[1]);
    ::close(err_pipe[0]);

    if (::dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
      int err = errno;
      if (::write(err_pipe[1], &err, sizeof(err)) < 0) {}
      ::_exit(127);
    }
    ::close(stdin_pipe[0]);

    int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::close(devnull);
    }

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(program));
    for (const char *a : args) argv.push_back(const_cast<char *>(a));
    argv.push_back(nullptr);

    std::signal(SIGPIPE, SIG_DFL);
    ::execvp(program, argv.data());
    int err = errno;
    if (::write(err_pipe[1], &err, sizeof(err)) < 0) {}
    ::_exit(127);
  }

  ::close(stdin_pipe[0]);
  ::close(err_pipe[1]);

  int child_errno = 0;
  ssize_t nread = ::read(err_pipe[0], &child_errno, sizeof(child_errno));
  ::close(err_pipe[0]);

  if (nread > 0) {
    ::close(stdin_pipe[1]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ErrKind k = (child_errno == ENOENT) ? ErrKind::NotFound : ErrKind::Other;
    return std::unexpected(make_err(k, std::strerror(child_errno)));
  }

  struct sigaction ign_sa{}, old_sa{};
  ign_sa.sa_handler = SIG_IGN;
  sigemptyset(&ign_sa.sa_mask);
  bool sig_guarded = (::sigaction(SIGPIPE, &ign_sa, &old_sa) == 0);
  struct SigpipeGuard {
    bool active;
    struct sigaction *old_act;
    ~SigpipeGuard() {
      if (active) ::sigaction(SIGPIPE, old_act, nullptr);
    }
  } sig_guard{sig_guarded, &old_sa};

  const char *ptr = content.data();
  size_t remaining = content.size();
  while (remaining > 0) {
    ssize_t written = ::write(stdin_pipe[1], ptr, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      int err = errno;
      ::close(stdin_pipe[1]);
      int status = 0;
      if (::waitpid(pid, &status, 0) >= 0 && err == EPIPE &&
          (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        std::string msg = "clipboard command `";
        msg.append(program).append("` failed (exit status: ");
        msg.append(
            std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1));
        msg.append("); see stderr above");
        return std::unexpected(make_err(ErrKind::Other, std::move(msg)));
      }
      return std::unexpected(make_err(ErrKind::Other, std::strerror(err)));
    }
    ptr += written;
    remaining -= static_cast<size_t>(written);
  }
  ::close(stdin_pipe[1]);

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    return std::unexpected(from_errno("waitpid"));
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return {};
  }
  std::string msg = "clipboard command `";
  msg.append(program).append("` failed (exit status: ");
  msg.append(std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1));
  msg.append("); see stderr above");
  return std::unexpected(make_err(ErrKind::Other, std::move(msg)));
}

Result<void> copy_to_clipboard(std::string_view content) {
  struct Backend {
    const char *program;
    std::vector<const char *> args;
  };
  std::vector<Backend> backends;

  bool is_wsl = has_env("WSL_DISTRO_NAME") || has_env("WSL_INTEROP");
  if (!is_wsl) {
    if (auto rel = read_file_to_string("/proc/sys/kernel/osrelease")) {
      std::string lower = ascii_lower(*rel);
      if (lower.find("microsoft") != std::string::npos) is_wsl = true;
    }
  }

  if (is_wsl) {
    backends.push_back({"clip.exe", {}});
  }
  if (has_env("WAYLAND_DISPLAY")) {
    backends.push_back({"wl-copy", {"--type", "text/plain;charset=utf-8"}});
  }
  if (has_env("DISPLAY")) {
    backends.push_back({"xclip", {"-selection", "clipboard"}});
    backends.push_back({"xsel", {"--clipboard", "--input"}});
  }

  for (const auto &b : backends) {
    auto res = run_clipboard_command(b.program, b.args, content);
    if (res) return {};
    if (res.error().kind == ErrKind::NotFound) continue;
    return std::unexpected(res.error());
  }

  return std::unexpected(make_err(
      ErrKind::NotFound,
      "no clipboard backend available: Wayland requires wl-copy (Ubuntu: sudo "
      "apt install wl-clipboard); X11 requires xclip or xsel; Windows/WSL "
      "requires clip.exe. Run in a desktop session with WAYLAND_DISPLAY or "
      "DISPLAY set, or enable WSL Windows interoperability."));
}

}  // namespace yrs::clipboard

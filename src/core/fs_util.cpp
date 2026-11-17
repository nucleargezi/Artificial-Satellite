#include "core/fs_util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace yrs {

Result<std::string> read_file_to_string(const fs::path &p) {
  std::FILE *fp = std::fopen(p.c_str(), "rb");
  if (!fp) return std::unexpected(from_errno(p.string()));
  std::string out;
  char buf[8192];
  while (size_t n = std::fread(buf, 1, sizeof(buf), fp)) {
    out.append(buf, n);
  }
  if (std::ferror(fp)) {
    int err = errno;
    std::fclose(fp);
    errno = err;
    return std::unexpected(from_errno(p.string()));
  }
  std::fclose(fp);
  return out;
}

Result<void> write_string_to_file(const fs::path &p, std::string_view content) {
  if (p.empty()) {
    return std::unexpected(make_err(ErrKind::InvalidInput, "empty file path"));
  }

  mode_t mode = 0666;
  bool has_orig_mode = false;
  struct stat st{};
  if (::stat(p.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      errno = EISDIR;
      return std::unexpected(from_errno(p.string()));
    }
    mode = st.st_mode & 07777;
    has_orig_mode = true;
  }

  fs::path parent = p.parent_path();
  if (parent.empty()) parent = ".";
  std::string tmp_tpl = (parent / ".yrs_tmp.XXXXXX").string();

  int fd = ::mkstemp(tmp_tpl.data());
  if (fd < 0) return std::unexpected(from_errno(p.string()));

  auto cleanup_fail = [&](int err) -> Result<void> {
    ::close(fd);
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(p.string()));
  };

  if (has_orig_mode) {
    if (::fchmod(fd, mode) != 0) return cleanup_fail(errno);
  } else {
    mode_t mask = ::umask(0);
    ::umask(mask);
    if (::fchmod(fd, mode & ~mask) != 0) return cleanup_fail(errno);
  }

  const char *ptr = content.data();
  size_t remaining = content.size();
  while (remaining > 0) {
    ssize_t n = ::write(fd, ptr, remaining);
    if (n < 0) {
      if (errno == EINTR) continue;
      return cleanup_fail(errno);
    }
    ptr += n;
    remaining -= static_cast<size_t>(n);
  }

  if (::fsync(fd) != 0 && errno != EINVAL && errno != EROFS) {
    return cleanup_fail(errno);
  }

  if (::close(fd) != 0) {
    int err = errno;
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(p.string()));
  }

  if (::rename(tmp_tpl.c_str(), p.c_str()) != 0) {
    int err = errno;
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(p.string()));
  }

  return {};
}

Result<void> copy_file_atomic(const fs::path &from, const fs::path &to) {
  int src_fd = ::open(from.c_str(), O_RDONLY);
  if (src_fd < 0) return std::unexpected(from_errno(from.string()));

  struct stat sb{};
  if (::fstat(src_fd, &sb) != 0) {
    int err = errno;
    ::close(src_fd);
    errno = err;
    return std::unexpected(from_errno(from.string()));
  }

  fs::path parent = to.parent_path();
  if (parent.empty()) parent = ".";
  std::string tmp_tpl = (parent / ".yrs_tmp.XXXXXX").string();

  int dst_fd = ::mkstemp(tmp_tpl.data());
  if (dst_fd < 0) {
    int err = errno;
    ::close(src_fd);
    errno = err;
    return std::unexpected(from_errno(to.string()));
  }

  auto cleanup_fail = [&](int err, const fs::path &ctx) -> Result<void> {
    ::close(src_fd);
    ::close(dst_fd);
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(ctx.string()));
  };

  if (::fchmod(dst_fd, sb.st_mode & 07777) != 0) {
    return cleanup_fail(errno, to);
  }

  char buf[16384];
  while (true) {
    ssize_t nread = ::read(src_fd, buf, sizeof(buf));
    if (nread < 0) {
      if (errno == EINTR) continue;
      return cleanup_fail(errno, from);
    }
    if (nread == 0) break;

    const char *ptr = buf;
    size_t remaining = static_cast<size_t>(nread);
    while (remaining > 0) {
      ssize_t nwritten = ::write(dst_fd, ptr, remaining);
      if (nwritten < 0) {
        if (errno == EINTR) continue;
        return cleanup_fail(errno, to);
      }
      ptr += nwritten;
      remaining -= static_cast<size_t>(nwritten);
    }
  }

  struct timespec ts[2] = {sb.st_atim, sb.st_mtim};
  (void)::futimens(dst_fd, ts);

  if (::fsync(dst_fd) != 0 && errno != EINVAL && errno != EROFS) {
    return cleanup_fail(errno, to);
  }

  ::close(src_fd);
  if (::close(dst_fd) != 0) {
    int err = errno;
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(to.string()));
  }

  if (::rename(tmp_tpl.c_str(), to.c_str()) != 0) {
    int err = errno;
    ::unlink(tmp_tpl.c_str());
    errno = err;
    return std::unexpected(from_errno(to.string()));
  }

  return {};
}

Result<void> move_file_across(const fs::path &from, const fs::path &to) {
  if (::rename(from.c_str(), to.c_str()) == 0) {
    return {};
  }
  if (errno == EXDEV) {
    auto cp_res = copy_file_atomic(from, to);
    if (!cp_res) return cp_res;
    if (::unlink(from.c_str()) != 0) {
      return std::unexpected(from_errno(from.string()));
    }
    return {};
  }
  return std::unexpected(from_errno(from.string()));
}

fs::path normalize_path(const fs::path &path) {
  std::error_code ec;
  fs::path norm = fs::weakly_canonical(path, ec);
  if (ec) {
    ec.clear();
    fs::path abs = fs::absolute(path, ec);
    norm = (ec ? path : abs).lexically_normal();
  }
  while (norm.has_relative_path() && norm.filename().empty()) {
    norm = norm.parent_path();
  }
  return norm;
}

bool path_starts_with(const fs::path &path, const fs::path &base) {
  auto pit = path.begin();
  for (const auto &b : base) {
    if (b.empty()) continue;
    while (pit != path.end() && pit->empty()) ++pit;
    if (pit == path.end() || *pit != b) return false;
    ++pit;
  }
  return true;
}

std::string rel_display(const fs::path &norm_base, const fs::path &p) {
  fs::path norm_p = normalize_path(p);
  if (norm_p == norm_base) return ".";
  if (path_starts_with(norm_p, norm_base)) {
    fs::path rel = norm_p.lexically_relative(norm_base);
    if (!rel.empty()) return rel.string();
  }
  return p.string();
}

void prune_empty_subdirs(const fs::path &root_dir, bool remove_root) {
  std::error_code ec;
  if (!fs::is_directory(root_dir, ec)) return;

  std::vector<fs::path> dirs;
  for (fs::recursive_directory_iterator it(root_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    auto st = it->symlink_status(ec);
    if (!ec && st.type() == fs::file_type::directory) {
      dirs.push_back(it->path());
    }
  }
  std::sort(dirs.begin(), dirs.end(), [](const fs::path &a, const fs::path &b) {
    return a.native().size() > b.native().size();
  });
  for (const auto &d : dirs) {
    if (fs::is_empty(d, ec) && !ec) {
      fs::remove(d, ec);
    }
  }
  if (remove_root && fs::is_empty(root_dir, ec) && !ec) {
    fs::remove(root_dir, ec);
  }
}

}  // namespace yrs

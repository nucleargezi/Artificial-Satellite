#include "cph/oj.hpp"

#include "core/str_util.hpp"

#include <cctype>
#include <cstdio>

namespace yrs::cph {

namespace {

std::optional<int64_t> extract_yukicoder_no_from_text(std::string_view text,
                                                      char sep) {
  for (size_t i = 0; i + 3 < text.size(); ++i) {
    if (i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1]))) {
      continue;
    }
    char c0 =
        static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
    char c1 = static_cast<char>(
        std::tolower(static_cast<unsigned char>(text[i + 1])));
    if (c0 == 'n' && c1 == 'o' && text[i + 2] == sep) {
      size_t p = i + 3;
      while (p < text.size() && text[p] == ' ') ++p;
      if (auto no = parse_leading_positive_i64(text.substr(p))) {
        return no;
      }
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<HttpUrl> parse_http_url(std::string_view url) {
  auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return std::nullopt;
  std::string scheme = ascii_lower(url.substr(0, scheme_end));
  if (scheme != "http" && scheme != "https") return std::nullopt;

  size_t auth_start = scheme_end + 3;
  std::string_view rest = url.substr(auth_start);
  size_t host_end = rest.find_first_of("/?#");
  std::string_view authority =
      (host_end == std::string_view::npos) ? rest : rest.substr(0, host_end);
  auto colon = authority.find(':');
  std::string host = ascii_lower(
      (colon == std::string_view::npos) ? authority : authority.substr(0, colon));
  if (host.empty()) return std::nullopt;

  std::string_view path;
  size_t path_offset = std::string_view::npos;
  if (host_end != std::string_view::npos && rest[host_end] == '/') {
    path_offset = auth_start + host_end;
    std::string_view tail = rest.substr(host_end);
    size_t qh = tail.find_first_of("?#");
    path = (qh == std::string_view::npos) ? tail : tail.substr(0, qh);
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
  }
  return HttpUrl{std::move(scheme), std::move(host), path, path_offset};
}

bool host_matches(std::string_view host, std::string_view domain) {
  if (host == domain) return true;
  if (host.size() <= domain.size()) return false;
  return host.ends_with(domain) &&
         host[host.size() - domain.size() - 1] == '.';
}

std::vector<std::string_view> split_path_segments(std::string_view path) {
  std::vector<std::string_view> segs;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    if (i >= path.size()) break;
    size_t start = i;
    while (i < path.size() && path[i] != '/') ++i;
    segs.push_back(path.substr(start, i - start));
  }
  return segs;
}

std::optional<std::string_view> find_query_param(std::string_view url,
                                                 std::string_view key) {
  auto q = url.find('?');
  if (q == std::string_view::npos) return std::nullopt;
  std::string_view qs = url.substr(q + 1);
  auto h = qs.find('#');
  if (h != std::string_view::npos) qs = qs.substr(0, h);

  size_t pos = 0;
  while (pos <= qs.size()) {
    auto amp = qs.find('&', pos);
    std::string_view pair = (amp == std::string_view::npos)
                                ? qs.substr(pos)
                                : qs.substr(pos, amp - pos);
    auto eq = pair.find('=');
    if (eq != std::string_view::npos && pair.substr(0, eq) == key) {
      return pair.substr(eq + 1);
    }
    if (amp == std::string_view::npos) break;
    pos = amp + 1;
  }
  return std::nullopt;
}

std::string clean_problem_url(std::string_view url) {
  auto parsed = parse_http_url(url);
  if (!parsed || parsed->path_offset == std::string_view::npos) {
    return std::string(url);
  }
  if (!host_matches(parsed->host, "qoj.ac")) {
    return std::string(url);
  }

  size_t path_start = parsed->path_offset;
  size_t path_end = url.find_first_of("?#", path_start);
  std::string_view raw_path = (path_end == std::string_view::npos)
                                  ? url.substr(path_start)
                                  : url.substr(path_start, path_end - path_start);

  struct Seg {
    std::string_view text;
    size_t end_off;
  };
  std::vector<Seg> segs;
  size_t i = 0;
  while (i < raw_path.size()) {
    while (i < raw_path.size() && raw_path[i] == '/') ++i;
    if (i >= raw_path.size()) break;
    size_t start = i;
    while (i < raw_path.size() && raw_path[i] != '/') ++i;
    segs.push_back(Seg{raw_path.substr(start, i - start), i});
  }

  size_t prefix_len = 0;
  if (segs.size() >= 5 && segs[0].text == "contest" &&
      all_ascii_digits(segs[1].text) && segs[2].text == "problem" &&
      all_ascii_digits(segs[3].text) && segs[4].text == "statement") {
    prefix_len = segs[3].end_off;
  } else if (segs.size() >= 3 && segs[0].text == "problem" &&
             all_ascii_digits(segs[1].text) && segs[2].text == "statement") {
    prefix_len = segs[1].end_off;
  }

  if (prefix_len > 0) {
    return std::string(url.substr(0, path_start + prefix_len));
  }
  return std::string(url);
}

std::optional<fs::path> route_single_problem(const fs::path &catalog_root,
                                             std::string_view filename,
                                             std::string_view name,
                                             std::string_view url) {
  if (filename.empty() || url.empty()) return std::nullopt;
  auto parsed = parse_http_url(url);
  if (!parsed) return std::nullopt;
  const std::string &host = parsed->host;
  std::string_view path = parsed->path;

  if (host_matches(host, "luogu.com.cn")) {
    if (!path.starts_with("/problem/")) return std::nullopt;
    std::string_view id = path.substr(9);
    if (id.size() >= 2 && id[0] == 'B' && all_ascii_digits(id.substr(1))) {
      return catalog_root / "luogu" / "B" / filename;
    }
    if (id.size() >= 2 && id[0] == 'P' && all_ascii_digits(id.substr(1))) {
      return catalog_root / "luogu" / filename;
    }
    return std::nullopt;
  }

  if (host_matches(host, "judge.yosupo.jp")) {
    if (!path.starts_with("/problem/")) return std::nullopt;
    std::string_view slug = path.substr(9);
    if (slug.empty() || slug.find('/') != std::string_view::npos) {
      return std::nullopt;
    }
    return catalog_root / "library_checker" / filename;
  }

  if (host_matches(host, "loj.ac")) {
    std::string_view id;
    if (path.starts_with("/p/")) id = path.substr(3);
    else if (path.starts_with("/problem/")) id = path.substr(9);
    else return std::nullopt;
    if (!all_ascii_digits(id)) return std::nullopt;
    return catalog_root / "loj" / filename;
  }

  if (host_matches(host, "uoj.ac")) {
    if (!path.starts_with("/problem/")) return std::nullopt;
    std::string_view id = path.substr(9);
    if (!all_ascii_digits(id)) return std::nullopt;
    return catalog_root / "uoj" / filename;
  }

  if (host_matches(host, "yukicoder.me")) {
    std::optional<int64_t> no;
    if (path.starts_with("/problems/no/")) {
      no = parse_positive_i64(path.substr(13));
    } else if (path.starts_with("/problems/") &&
               all_ascii_digits(path.substr(10))) {
      no = extract_yukicoder_no_from_text(name, '.');
      if (!no) no = extract_yukicoder_no_from_text(filename, '_');
    }
    if (!no || *no <= 0) return std::nullopt;

    std::string bucket;
    if (*no < 100) {
      bucket = "0001-0100";
    } else {
      int64_t lo = (*no / 100) * 100;
      int64_t hi = lo + 100;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%04lld-%04lld",
                    static_cast<long long>(lo), static_cast<long long>(hi));
      bucket = buf;
    }
    return catalog_root / "yukicoder" / bucket / filename;
  }

  return std::nullopt;
}

std::optional<ContestInfo> extract_contest_info(std::string_view url) {
  auto parsed = parse_http_url(url);
  if (!parsed) return std::nullopt;
  const std::string &host = parsed->host;
  auto segs = split_path_segments(parsed->path);

  if (host_matches(host, "qoj.ac")) {
    if (segs.size() >= 4 && segs[0] == "contest" && all_ascii_digits(segs[1]) &&
        segs[2] == "problem" && all_ascii_digits(segs[3])) {
      std::string sid(segs[1]);
      return ContestInfo{"qoj:" + sid, sid, "qoj"};
    }
    return std::nullopt;
  }

  if (host_matches(host, "codeforces.com")) {
    if (segs.size() >= 4 && segs[0] == "contest" && all_ascii_digits(segs[1]) &&
        segs[2] == "problem" && !segs[3].empty()) {
      std::string sid(segs[1]);
      return ContestInfo{"cf:" + sid, sid, "cf"};
    }
    if (segs.size() >= 4 && segs[0] == "gym" && all_ascii_digits(segs[1]) &&
        segs[2] == "problem" && !segs[3].empty()) {
      std::string sid(segs[1]);
      return ContestInfo{"gym:" + sid, sid, "gym"};
    }
    return std::nullopt;
  }

  if (host_matches(host, "atcoder.jp")) {
    if (segs.size() >= 4 && segs[0] == "contests" && !segs[1].empty() &&
        segs[2] == "tasks" && !segs[3].empty()) {
      std::string sid = ascii_lower(segs[1]);
      return ContestInfo{"atcoder:" + sid, sid, "atcoder"};
    }
    return std::nullopt;
  }

  if (host_matches(host, "uoj.ac")) {
    if (segs.size() >= 4 && segs[0] == "contest" && all_ascii_digits(segs[1]) &&
        segs[2] == "problem" && all_ascii_digits(segs[3])) {
      std::string sid(segs[1]);
      return ContestInfo{"uoj:" + sid, sid, "uoj"};
    }
    return std::nullopt;
  }

  if (host_matches(host, "ac.nowcoder.com")) {
    if (segs.size() >= 4 && segs[0] == "acm" && segs[1] == "contest" &&
        all_ascii_digits(segs[2]) && !segs[3].empty()) {
      std::string sid(segs[2]);
      return ContestInfo{"nowcoder:" + sid, sid, "nowcoder"};
    }
    return std::nullopt;
  }

  if (host_matches(host, "luogu.com.cn")) {
    if (auto cid = find_query_param(url, "contestId")) {
      if (all_ascii_digits(*cid)) {
        std::string sid(*cid);
        return ContestInfo{"luogu:" + sid, sid, "luogu"};
      }
    }
    return std::nullopt;
  }

  return std::nullopt;
}

std::string extract_problem_label(std::string_view name, std::string_view url,
                                  std::string_view stem) {
  std::string_view n = trim(name);

  if (ascii_icase_starts_with(n, "ex")) {
    size_t j = 2;
    while (j < n.size() && std::isdigit(static_cast<unsigned char>(n[j]))) ++j;
    std::string_view rest = n.substr(j);
    if (rest.empty() || rest[0] == ' ' || rest[0] == '\t' || rest[0] == '.' ||
        rest[0] == ':' || rest[0] == '-' || rest[0] == ')' || rest[0] == ']' ||
        rest.starts_with("\xE2\x80\x93") || rest.starts_with("\xE2\x80\x94")) {
      return "Ex" + std::string(n.substr(2, j - 2));
    }
  }

  if (!n.empty() && std::isalpha(static_cast<unsigned char>(n[0]))) {
    bool all_rest_digits = true;
    for (size_t i = 1; i < n.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(n[i]))) {
        all_rest_digits = false;
        break;
      }
    }
    if (all_rest_digits) return ascii_upper(n);
  }

  {
    std::string_view s = n;
    bool had_prefix = false;
    if (ascii_icase_starts_with(s, "problem") && s.size() > 7 &&
        (s[7] == ' ' || s[7] == '\t')) {
      s = trim_start(s.substr(7));
      had_prefix = true;
    }
    bool bracketed = false;
    if (!s.empty() && s[0] == '[') {
      s.remove_prefix(1);
      bracketed = true;
    }
    if (!s.empty() && std::isalpha(static_cast<unsigned char>(s[0]))) {
      size_t j = 1;
      while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) {
        ++j;
      }
      std::string_view cand = s.substr(0, j);
      std::string_view rest = s.substr(j);
      if (bracketed) {
        if (!rest.empty() && rest[0] == ']') {
          return ascii_upper(cand);
        }
      } else {
        std::string_view r2 = trim_start(rest);
        bool has_punct = !r2.empty() &&
                         (r2[0] == '.' || r2[0] == ':' || r2[0] == '-' ||
                          r2[0] == ')' || r2[0] == ']' ||
                          r2.starts_with("\xE2\x80\x93") ||
                          r2.starts_with("\xE2\x80\x94"));
        if (has_punct) return ascii_upper(cand);
        bool upper_or_prefix = had_prefix || (s[0] >= 'A' && s[0] <= 'Z');
        if (upper_or_prefix && !rest.empty() &&
            (rest[0] == ' ' || rest[0] == '\t')) {
          return ascii_upper(cand);
        }
      }
    }
  }

  if (auto parsed = parse_http_url(url)) {
    const std::string &host = parsed->host;
    auto segs = split_path_segments(parsed->path);
    if (host_matches(host, "codeforces.com")) {
      if (segs.size() >= 4 && (segs[0] == "contest" || segs[0] == "gym") &&
          segs[2] == "problem" && !segs[3].empty()) {
        return ascii_upper(segs[3]);
      }
    } else if (host_matches(host, "ac.nowcoder.com")) {
      if (segs.size() >= 4 && segs[0] == "acm" && segs[1] == "contest" &&
          !segs[3].empty()) {
        return ascii_upper(segs[3]);
      }
    } else if (host_matches(host, "atcoder.jp")) {
      if (segs.size() >= 4 && segs[0] == "contests" && segs[2] == "tasks" &&
          !segs[3].empty()) {
        auto us = segs[3].rfind('_');
        if (us != std::string_view::npos && us + 1 < segs[3].size()) {
          std::string_view tag = segs[3].substr(us + 1);
          if (ascii_icase_starts_with(tag, "ex") &&
              (tag.size() == 2 || all_ascii_digits(tag.substr(2)))) {
            return "Ex" + std::string(tag.substr(2));
          }
          if (tag.size() == 1 && tag[0] >= '1' && tag[0] <= '9') {
            return std::string(1, static_cast<char>('A' + (tag[0] - '1')));
          }
          return ascii_upper(tag);
        }
      }
    }
  }

  return std::string(stem);
}

std::string prepend_url_comment_if_missing(std::string_view content,
                                           std::string_view url) {
  if (url.empty()) return std::string(content);
  std::string_view inspect = content;
  if (inspect.starts_with("\xEF\xBB\xBF")) inspect.remove_prefix(3);
  auto nl = inspect.find('\n');
  std::string_view first_line =
      (nl == std::string_view::npos) ? inspect : inspect.substr(0, nl);
  first_line = trim_start(first_line);
  if (first_line.starts_with("//") &&
      (first_line.find("http://") != std::string_view::npos ||
       first_line.find("https://") != std::string_view::npos)) {
    return std::string(content);
  }
  std::string out;
  out.reserve(url.size() + 4 + content.size());
  out.append("// ").append(url).push_back('\n');
  out.append(content);
  return out;
}

}  // namespace yrs::cph

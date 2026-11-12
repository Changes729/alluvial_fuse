// util.cpp — oofs 基础工具实现
#include "util.hpp"

#include "overlay.hpp"  // 磁盘格式常量（kOpaqueMarker 等）

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/xattr.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace alluvial_fuse {

// ---------- 路径 ----------
std::string absPath(const std::string& p) {
  if (p.empty()) return "/";
  std::string base;
  if (p[0] == '/') {
    base = p;
  } else {
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) buf[0] = '\0';
    base = std::string(buf) + "/" + p;
  }
  std::vector<std::string> parts;
  for (const auto& seg : split(base, '/')) {
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(seg);
  }
  std::string out;
  for (const auto& s : parts) out += "/" + s;
  return out.empty() ? "/" : out;
}

std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

bool pathUnder(const std::string& rel, const std::string& prefix) {
  if (prefix.empty()) return true;
  if (rel == prefix) return true;
  return rel.size() > prefix.size() + 1 && rel.compare(0, prefix.size(), prefix) == 0 &&
         rel[prefix.size()] == '/';
}

// ---------- 文件 ----------
std::optional<struct stat> lstatOpt(const std::string& path) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) return std::nullopt;
  return st;
}

bool isCharWhiteout(const struct stat& st) {
  return S_ISCHR(st.st_mode) && st.st_rdev == 0;
}

bool isWhiteoutEntry(const std::string& name, const struct stat& st) {
  if (name == kOpaqueMarkerName()) return false;
  if (isCharWhiteout(st)) return true;
  // mknod 失败时的回退格式：普通文件 .wh.<name>
  return S_ISREG(st.st_mode) && name.rfind(kWhiteoutPrefixName(), 0) == 0;
}

std::string whiteoutTargetName(const std::string& name) {
  if (name.rfind(kWhiteoutPrefixName(), 0) == 0) return name.substr(4);
  return name;
}

bool hasXattrValue(const std::string& path, const char* name) {
  return ::getxattr(path.c_str(), name, nullptr, 0) > 0;
}

bool readXattrValue(const std::string& path, const char* name, std::string& out) {
  ssize_t n = ::getxattr(path.c_str(), name, nullptr, 0);
  if (n <= 0) return false;
  std::string buf(static_cast<size_t>(n), '\0');
  n = ::getxattr(path.c_str(), name, buf.data(), buf.size());
  if (n <= 0) return false;
  buf.resize(static_cast<size_t>(n));
  // 字符串值去掉结尾 NUL；二进制句柄（trusted.overlay.origin）原样返回
  while (!buf.empty() && buf.back() == '\0') buf.pop_back();
  out = std::move(buf);
  return true;
}

std::vector<std::string> listXattrs(const std::string& path) {
  std::vector<std::string> out;
  ssize_t n = ::llistxattr(path.c_str(), nullptr, 0);
  if (n <= 0) return out;
  std::string buf(static_cast<size_t>(n), '\0');
  n = ::llistxattr(path.c_str(), buf.data(), buf.size());
  if (n <= 0) return out;
  size_t off = 0;
  while (off < static_cast<size_t>(n)) {
    const char* entry = buf.c_str() + off;
    size_t len = std::strlen(entry) + 1;
    out.emplace_back(entry);
    off += len;
  }
  return out;
}

std::vector<std::string> listDir(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return out;  // 目录不存在（或不是目录）→ 视为空
  while (auto* e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
    out.emplace_back(e->d_name);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

bool readFile(const std::string& path, std::string& out) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  out.clear();
  char buf[65536];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
  ::close(fd);
  return n == 0;
}

bool writeFile(const std::string& path, const std::string& content) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  size_t off = 0;
  bool ok = true;
  while (off < content.size()) {
    ssize_t w = ::write(fd, content.data() + off, content.size() - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    off += static_cast<size_t>(w);
  }
  ::close(fd);
  return ok;
}

std::string readSymlink(const std::string& path) {
  char buf[4096];
  ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (n < 0) return "";
  buf[n] = '\0';
  return buf;
}

bool mkdirs(const std::string& path, mode_t mode) {
  if (path.empty() || path == "/") return true;
  std::string cur;
  for (const auto& seg : split(path, '/')) {
    if (seg.empty()) continue;
    cur += "/" + seg;
    if (::mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) return false;
  }
  return true;
}

bool removeEntry(const std::string& path, bool isDir) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) return errno == ENOENT;
  if (S_ISDIR(st.st_mode) || isDir) {
    bool ok = true;
    for (const auto& name : listDir(path)) {
      std::string child = joinPath(path, name);
      struct stat cst {};
      if (::lstat(child.c_str(), &cst) != 0) continue;
      ok = removeEntry(child, S_ISDIR(cst.st_mode)) && ok;
    }
    return ::rmdir(path.c_str()) == 0 && ok;
  }
  return ::unlink(path.c_str()) == 0;
}

bool parentDirOf(const std::string& path, std::string& out) {
  auto pos = path.rfind('/');
  if (pos == std::string::npos) return false;
  out = path.substr(0, pos);
  return !out.empty();
}

// ---------- 进程 ----------
std::string findExecutable(const std::string& name, const std::vector<std::string>& extraPaths) {
  auto usable = [](const std::string& p) {
    return ::access(p.c_str(), X_OK) == 0;
  };
  if (name.find('/') != std::string::npos) return usable(name) ? name : "";
  for (const auto& p : extraPaths) {
    if (usable(p)) return p;
  }
  const char* pathEnv = ::getenv("PATH");
  if (pathEnv) {
    for (const auto& dir : split(pathEnv, ':')) {
      if (dir.empty()) continue;
      std::string cand = joinPath(dir, name);
      if (usable(cand)) return cand;
    }
  }
  return "";
}

// ---------- /proc/mounts ----------
std::string unescapeMount(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == '0') {
      int code = (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
      out += static_cast<char>(code);
      i += 3;
    } else {
      out += s[i];
    }
  }
  return out;
}

bool isMounted(const std::string& mountPoint) {
  std::ifstream f("/proc/mounts");
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    std::string dev, mnt, type;
    ss >> dev >> mnt >> type;
    if (unescapeMount(mnt) == mountPoint &&
        (type.rfind("fuse", 0) == 0 || type == "overlay")) {
      return true;
    }
  }
  return false;
}

// ---------- 其他 ----------
std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

std::string basenameOf(const std::string& path) {
  auto pos = path.rfind('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

}  // namespace oofs

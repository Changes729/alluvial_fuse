// atfs.cpp — 基于 dirfd 的文件操作实现
#include "atfs.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>

#include "util.hpp"

namespace alluvial_fuse {

std::optional<struct stat> lstatAt(int dirfd, const std::string& rel) {
  // rel == "" 表示 dirfd 本身（根目录）：fstatat 的空相对路径非法，需映射为 "."
  std::string r = rel.empty() ? "." : rel;
  struct stat st {};
  if (::fstatat(dirfd, r.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) return std::nullopt;
  return st;
}

bool existsAt(int dirfd, const std::string& rel) {
  std::string r = rel.empty() ? "." : rel;
  struct stat st {};
  return ::fstatat(dirfd, r.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
}

std::vector<std::string> listDirAt(int dirfd, const std::string& rel) {
  std::vector<std::string> out;
  // 注意：不能用 dup() —— dup 共享文件描述（目录偏移量会被 readdir 推进，
  // 第二次枚举从末尾开始得到空列表）。必须 openat 打开全新描述。
  std::string r = rel.empty() ? "." : rel;
  int fd = ::openat(dirfd, r.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return out;
  DIR* d = ::fdopendir(fd);
  if (!d) {
    ::close(fd);
    return out;
  }
  while (auto* e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
    out.emplace_back(e->d_name);
  }
  ::closedir(d);  // 连同 fd 一起关闭
  std::sort(out.begin(), out.end());
  return out;
}

bool readFileAt(int dirfd, const std::string& rel, std::string& out) {
  std::string r = rel.empty() ? "." : rel;
  int fd = ::openat(dirfd, r.c_str(), O_RDONLY);
  if (fd < 0) return false;
  out.clear();
  char buf[65536];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
  ::close(fd);
  return n == 0;
}

std::string readSymlinkAt(int dirfd, const std::string& rel) {
  std::string r = rel.empty() ? "." : rel;
  char buf[4096];
  ssize_t n = ::readlinkat(dirfd, r.c_str(), buf, sizeof(buf) - 1);
  if (n < 0) return "";
  buf[n] = '\0';
  return buf;
}

bool mkdirsAt(int dirfd, const std::string& rel, mode_t mode) {
  if (rel.empty()) return true;
  std::string cur;
  for (const auto& seg : split(rel, '/')) {
    if (seg.empty()) continue;
    cur += "/" + seg;
    if (::mkdirat(dirfd, cur.c_str() + 1, mode) != 0 && errno != EEXIST) return false;
  }
  return true;
}

bool removeEntryAt(int dirfd, const std::string& rel, bool isDir) {
  std::string r = rel.empty() ? "." : rel;
  struct stat st {};
  if (::fstatat(dirfd, r.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
  if (S_ISDIR(st.st_mode) || isDir) {
    bool ok = true;
    for (const auto& name : listDirAt(dirfd, rel)) {
      std::string child = rel.empty() ? name : rel + "/" + name;
      struct stat cst {};
      if (::fstatat(dirfd, child.c_str(), &cst, AT_SYMLINK_NOFOLLOW) != 0) continue;
      ok = removeEntryAt(dirfd, child, S_ISDIR(cst.st_mode)) && ok;
    }
    return ::unlinkat(dirfd, rel.c_str(), AT_REMOVEDIR) == 0 && ok;
  }
  return ::unlinkat(dirfd, rel.c_str(), 0) == 0;
}

// ---------- xattr ----------

int openPathAt(int dirfd, const std::string& rel) {
  std::string r = rel.empty() ? "." : rel;
  return ::openat(dirfd, r.c_str(), O_PATH | O_NOFOLLOW);
}

// xattr 专用：O_PATH fd 上 fgetxattr/fsetxattr 返回 EBADF（实测），必须真实打开。
// user.* xattr 仅 regular file / directory 支持；symlink/fifo/dev 无 xattr 语义。
// O_NONBLOCK 防止误开 FIFO 阻塞。
static int openAttrFdAt(int dirfd, const std::string& rel) {
  std::string r = rel.empty() ? "." : rel;
  struct stat st {};
  if (::fstatat(dirfd, r.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) return -1;
  if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return -1;
  int flags = S_ISDIR(st.st_mode) ? (O_RDONLY | O_DIRECTORY) : (O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  return ::openat(dirfd, r.c_str(), flags);
}

bool hasXattrAt(int dirfd, const std::string& rel, const char* name) {
  int fd = openAttrFdAt(dirfd, rel);
  if (fd < 0) return false;
  ssize_t n = ::fgetxattr(fd, name, nullptr, 0);
  ::close(fd);
  return n > 0;
}

bool readXattrAt(int dirfd, const std::string& rel, const char* name, std::string& out) {
  int fd = openAttrFdAt(dirfd, rel);
  if (fd < 0) return false;
  ssize_t n = ::fgetxattr(fd, name, nullptr, 0);
  if (n > 0) {
    std::string buf(static_cast<size_t>(n), '\0');
    n = ::fgetxattr(fd, name, buf.data(), buf.size());
    if (n > 0) {
      buf.resize(static_cast<size_t>(n));
      while (!buf.empty() && buf.back() == '\0') buf.pop_back();
      out = std::move(buf);
    }
  }
  ::close(fd);
  return n > 0;
}

bool writeXattrAt(int dirfd, const std::string& rel, const char* name, const std::string& value) {
  int fd = openAttrFdAt(dirfd, rel);
  if (fd < 0) return false;
  bool ok = ::fsetxattr(fd, name, value.data(), value.size(), 0) == 0;
  ::close(fd);
  return ok;
}

bool removeXattrAt(int dirfd, const std::string& rel, const char* name) {
  int fd = openAttrFdAt(dirfd, rel);
  if (fd < 0) return false;
  bool ok = ::fremovexattr(fd, name) == 0;
  ::close(fd);
  return ok;
}

std::vector<std::string> listXattrsAt(int dirfd, const std::string& rel) {
  std::vector<std::string> out;
  int fd = openAttrFdAt(dirfd, rel);
  if (fd < 0) return out;
  ssize_t n = ::flistxattr(fd, nullptr, 0);
  if (n > 0) {
    std::string buf(static_cast<size_t>(n), '\0');
    n = ::flistxattr(fd, buf.data(), buf.size());
    if (n > 0) {
      size_t off = 0;
      while (off < static_cast<size_t>(n)) {
        const char* entry = buf.c_str() + off;
        size_t len = std::strlen(entry) + 1;
        out.emplace_back(entry);
        off += len;
      }
    }
  }
  ::close(fd);
  return out;
}

}  // namespace oofs

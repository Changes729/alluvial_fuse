// overlay.cpp — 覆盖层模型实现（fd 相对访问）
#include "overlay.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "atfs.hpp"
#include "util.hpp"

namespace alluvial_fuse {

void Overlay::init(OverlayConfig &&cfg_) {
  cfg = std::move(cfg_);

  struct Target {
    std::string path;
    int *fd;
    const char *what;
  } targets[] = {
      {cfg.mount.string(), &lowerFd_, "lowerdir"},
      {cfg.upper.string(), &upperFd_, "upperdir"},
  };

  // 先按路径校验（此时尚未挂载）
  for (const auto &t : targets) {
    auto st = lstatOpt(t.path);

    if (!st) {
      std::error_code ec;
      fs::create_directories(t.path, ec);
      if (ec) {
        throw runtime_error(t.path + "创建失败: " + ec.message());
      }
    } else if (!S_ISDIR(st->st_mode)) {
      throw invalid_argument("路径 " + t.path + " 不是目录");
    }
  }

  // 预打开目录 fd：挂载后一切访问必须走 fd 相对路径
  for (const auto &t : targets) {
    *t.fd = ::open(t.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (*t.fd < 0) {
      throw runtime_error(string("打开 ") + t.what + " 失败: " + t.path + ": " +
                          strerror(errno));
    }
  }
}

std::optional<struct stat> Overlay::lowerStat(const std::string &rel) const {
  return lstatAt(lowerFd_, rel);
}

std::optional<struct stat> Overlay::upperStat(const std::string &rel) const {
  return lstatAt(upperFd_, rel);
}

bool Overlay::lowerExists(const std::string &rel) const {
  return existsAt(lowerFd_, rel);
}

bool Overlay::upperExists(const std::string &rel) const {
  return existsAt(upperFd_, rel);
}

bool Overlay::isWhiteout(const std::string &rel) const {
  auto st = upperStat(rel);
  if (!st)
    return false;
  std::string name = rel.substr(rel.rfind('/') + 1);
  return isWhiteoutEntry(name, *st);
}

bool Overlay::isOpaqueDir(const std::string &rel) const {
  // 双识别：fuse-overlayfs 非特权 xattr / 标记文件；特权模式
  // trusted.overlay.opaque
  std::string v;
  if (readXattrAt(upperFd_, rel, kOpaqueXattr, v) && v == "y")
    return true;
  if (readXattrAt(upperFd_, rel, kOpaqueXattrPriv, v) && v == "y")
    return true;
  return existsAt(upperFd_,
                  rel.empty() ? kOpaqueMarker : rel + "/" + kOpaqueMarker);
}

Layer Overlay::resolve(const std::string &rel, struct stat *outSt) const {
  auto ust = upperStat(rel);
  if (ust) {
    std::string name = rel.substr(rel.rfind('/') + 1);
    if (!rel.empty() && isWhiteoutEntry(name, *ust))
      return Layer::None;
    if (!rel.empty() && name == kOpaqueMarker)
      return Layer::None;
    if (outSt)
      *outSt = *ust;
    return Layer::Upper;
  }
  // mknod 不可用时的回退白名单：upper 有 .wh.<name> 时视为已删除
  {
    std::string name = rel.substr(rel.rfind('/') + 1);
    if (!rel.empty() && name.rfind(kWhiteoutPrefixName(), 0) != 0) {
      auto pos = rel.rfind('/');
      std::string parent = (pos == std::string::npos) ? "" : rel.substr(0, pos);
      std::string whName = std::string(kWhiteoutPrefixName()) + name;
      auto wst = upperStat(parent.empty() ? whName : parent + "/" + whName);
      if (wst && S_ISREG(wst->st_mode))
        return Layer::None;
    }
  }
  auto lst = lowerStat(rel);
  if (lst) {
    if (outSt)
      *outSt = *lst;
    return Layer::Lower;
  }
  return Layer::None;
}

std::vector<Overlay::Entry> Overlay::mergedList(const std::string &rel) const {
  std::vector<Entry> out;
  const bool opaque = isOpaqueDir(rel);

  std::vector<std::pair<std::string, Layer>> merged;
  auto covered = [&merged](const std::string &n) -> bool {
    for (const auto &[name, layer] : merged) {
      if (name == n)
        return true; // upper 副本或白名单占位（层为 None）都视为已覆盖
    }
    return false;
  };

  for (const auto &name : listDirAt(upperFd_, rel)) {
    if (name == kOpaqueMarker)
      continue; // 元数据，不是内容
    auto st = lstatAt(upperFd_, rel.empty() ? name : rel + "/" + name);
    if (!st)
      continue;
    if (isWhiteoutEntry(name, *st)) {
      // 白名单：占位隐藏 lower；.wh.<name> 回退格式映射回真实条目名
      merged.emplace_back(whiteoutTargetName(name), Layer::None);
      continue;
    }
    merged.emplace_back(name, Layer::Upper);
  }
  if (!opaque) {
    for (const auto &name : listDirAt(lowerFd_, rel)) {
      if (covered(name))
        continue;
      merged.emplace_back(name, Layer::Lower);
    }
  }

  for (const auto &[name, layer] : merged) {
    if (layer == Layer::None)
      continue;
    out.push_back(Entry{name, layer});
  }
  std::sort(out.begin(), out.end(),
            [](const Entry &a, const Entry &b) { return a.name < b.name; });
  return out;
}

bool Overlay::ensureUpperParent(const std::string &rel,
                                std::string &err) const {
  auto pos = rel.rfind('/');
  if (pos == std::string::npos)
    return true; // 根级条目
  if (!mkdirsAt(upperFd_, rel.substr(0, pos))) {
    err = "创建 upper 父目录失败: " + rel.substr(0, pos);
    return false;
  }
  return true;
}

bool Overlay::copyUp(const std::string &rel, std::string &err) const {
  auto lst = lowerStat(rel);
  if (!lst) {
    err = "copyUp: lower 条目不存在: " + rel;
    return false;
  }
  if (!ensureUpperParent(rel, err))
    return false;

  if (S_ISDIR(lst->st_mode)) {
    if (::mkdirat(upperFd_, rel.c_str(), lst->st_mode & 07777) != 0 &&
        errno != EEXIST) {
      err = "copyUp mkdirat(" + rel + "): " + std::strerror(errno);
      return false;
    }
    // chmod + 时间戳（尽力而为）
    if (int fd = ::openat(upperFd_, rel.c_str(), O_RDONLY | O_DIRECTORY);
        fd >= 0) {
      ::fchmod(fd, lst->st_mode & 07777);
      struct timespec ts[2] = {lst->st_atim, lst->st_mtim};
      ::futimens(fd, ts);
      ::close(fd);
    }
    return true;
  }
  if (S_ISLNK(lst->st_mode)) {
    // 与 fuse-overlayfs 一致：符号链接 copy-up 不写 origin xattr
    ::unlinkat(upperFd_, rel.c_str(), 0); // 若存在残余
    std::string target = readSymlinkAt(lowerFd_, rel);
    if (::symlinkat(target.c_str(), upperFd_, rel.c_str()) != 0) {
      err = "copyUp symlinkat(" + rel + " -> " + target +
            "): " + std::strerror(errno);
      return false;
    }
    return true;
  }
  if (S_ISREG(lst->st_mode)) {
    int src = ::openat(lowerFd_, rel.c_str(), O_RDONLY);
    if (src < 0) {
      err = "copyUp openat lower(" + rel + "): " + std::strerror(errno);
      return false;
    }
    int dst =
        ::openat(upperFd_, rel.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (dst < 0 && errno == EEXIST) {
      // upper 已有副本（并发 copy-up 等）→ 直接视为完成
      ::close(src);
      return true;
    }
    if (dst < 0) {
      err = "copyUp openat upper(" + rel + "): " + std::strerror(errno);
      ::close(src);
      return false;
    }
    char buf[65536];
    ssize_t n;
    bool ok = true;
    while ((n = ::read(src, buf, sizeof(buf))) != 0) {
      if (n < 0) {
        if (errno == EINTR)
          continue;
        ok = false;
        break;
      }
      ssize_t off = 0;
      while (off < n) {
        ssize_t w = ::write(dst, buf + off, static_cast<size_t>(n - off));
        if (w < 0) {
          if (errno == EINTR)
            continue;
          ok = false;
          break;
        }
        off += w;
      }
      if (!ok)
        break;
    }
    ::close(src);
    if (ok) {
      ::fchmod(dst, lst->st_mode & 07777);
      struct timespec ts[2] = {lst->st_atim, lst->st_mtim};
      ::futimens(dst, ts);
      // origin xattr：标记该条目由提交态 copy-up 而来（ogit 据此推断 modified）
      ::fsetxattr(dst, kOriginXattr, rel.c_str(), rel.size(), 0);
    }
    ::close(dst);
    if (!ok)
      err = "copyUp write(" + rel + ") 失败";
    return ok;
  }
  // 特殊文件（fifo/设备/socket）
  if (::mknodat(upperFd_, rel.c_str(), lst->st_mode & (S_IFMT | 07777),
                lst->st_rdev) != 0) {
    err = "copyUp mknodat(" + rel + "): " + std::strerror(errno);
    return false;
  }
  return true;
}

bool Overlay::makeWhiteout(const std::string &rel, std::string &err) const {
  if (!ensureUpperParent(rel, err))
    return false;
  // 首选 fuse-overlayfs / 内核 overlayfs 约定：字符设备 0:0
  if (::mknodat(upperFd_, rel.c_str(), S_IFCHR | 0600, makedev(0, 0)) == 0)
    return true;
  if (errno == EEXIST) {
    removeWhiteout(rel);
    if (::mknodat(upperFd_, rel.c_str(), S_IFCHR | 0600, makedev(0, 0)) == 0)
      return true;
  }
  if (errno != EPERM) {
    err = "mknodat whiteout(" + rel + "): " + std::strerror(errno);
    return false;
  }
  // 回退格式：普通文件 .wh.<name>
  std::string name = rel.substr(rel.rfind('/') + 1);
  std::string parentRel = rel.substr(0, rel.rfind('/'));
  std::string fb = parentRel.empty() ? std::string(kWhiteoutPrefix) + name
                                     : parentRel + "/" + kWhiteoutPrefix + name;
  int fd = ::openat(upperFd_, fb.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    err = "创建回退白名单(" + fb + "): " + std::strerror(errno);
    return false;
  }
  ::close(fd);
  return true;
}

void Overlay::removeWhiteout(const std::string &rel) const {
  auto st = upperStat(rel);
  if (!st)
    return;
  std::string name = rel.substr(rel.rfind('/') + 1);
  if (isWhiteoutEntry(name, *st))
    ::unlinkat(upperFd_, rel.c_str(), 0);
}

bool Overlay::markOpaque(const std::string &rel, std::string &err) const {
  // 双写：标记文件（ogit hasOpaqueMarker 识别）+ xattr（ogit isOpaqueDir 识别）
  std::string marker = rel.empty() ? kOpaqueMarker : rel + "/" + kOpaqueMarker;
  int fd =
      ::openat(upperFd_, marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    err = "写 opaque 标记失败(" + marker + "): " + std::strerror(errno);
    return false;
  }
  ::close(fd);
  writeXattrAt(upperFd_, rel, kOpaqueXattr, "y"); // 尽力而为
  return true;
}

void Overlay::clearOpaque(const std::string &rel) const {
  std::string marker = rel.empty() ? kOpaqueMarker : rel + "/" + kOpaqueMarker;
  ::unlinkat(upperFd_, marker.c_str(), 0);
  removeXattrAt(upperFd_, rel, kOpaqueXattr);     // 尽力而为
  removeXattrAt(upperFd_, rel, kOpaqueXattrPriv); // 尽力而为
}

} // namespace alluvial_fuse

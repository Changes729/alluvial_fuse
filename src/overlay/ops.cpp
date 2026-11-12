// ops.cpp — FUSE 高级别操作实现（合并视图 → lower/upper 分发 + copy-up）
//
// 并发模型：
//   - fuse_loop 单线程处理 FUSE 请求；TCP 控制线程独立运行。
//   - g_stateMutex 同时保护 FUSE ops 与控制平面提交，提交期间 FUSE
//   请求短暂冻结。
//   - 文件句柄编码：fh = fd | (O_APPEND 标记 << 32)。
//
// IO 模型：一切 lower/upper 访问经预打开的目录 fd 用 *at()
// 完成（atfs/overlay），
// 永不按路径访问挂载点覆盖的目录——否则守护进程会等待自己答复而死锁。
#include "ops.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "atfs.hpp"
#include "overlay.hpp"
#include "util.hpp"

namespace alluvial_fuse {

// 全局状态锁（server.cpp 的控制平面同样使用）
std::mutex g_stateMutex;
function<Overlay &()> Ops::_getOverlay;

std::atomic<int> g_openCount{0};
int Ops::openFileCount() { return g_openCount.load(); }

namespace {

constexpr uint64_t kFhAppendBit = 1ULL << 32;

std::string relOf(const char *path) {
  std::string p(path);
  if (!p.empty() && p[0] == '/')
    p.erase(0, 1);
  while (p.size() > 1 && p.back() == '/')
    p.pop_back();
  return p;
}

bool isInternalXattr(const char *name) {
  return std::strncmp(name, "user.fuseoverlayfs.", 19) == 0 ||
         std::strncmp(name, "trusted.overlay.", 16) == 0;
}

int errnoErr() { return -errno; }

// copy-up 当写入需要（lower-only 且带写意图）
int copyUpIfNeeded(const std::string &rel, Layer l) {
  if (l == Layer::Lower) {
    std::string err;
    if (!Ops::overlay().copyUp(rel, err)) {
      std::fprintf(stderr, "[fuse] copyUp(%s): %s\n", rel.c_str(), err.c_str());
      return -EIO;
    }
  }
  return 0;
}

// ---------- 操作实现（调用方已持锁） ----------

int impl_getattr(const std::string &rel, struct stat *st,
                 struct fuse_file_info *fi) {
  if (fi && fi->fh) { // 已打开文件的 fstat 语义
    int fd = static_cast<int>(fi->fh & 0xFFFFFFFF);
    if (::fstat(fd, st) != 0)
      return errnoErr();
    return 0;
  }
  struct stat tmp{};
  if (Ops::overlay().resolve(rel, &tmp) == Layer::None)
    return -ENOENT;
  *st = tmp;
  return 0;
}

int impl_readlink(const std::string &rel, char *buf, size_t size) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  if (!S_ISLNK(st.st_mode))
    return -EINVAL;
  int base =
      l == Layer::Upper ? Ops::overlay().upperFd() : Ops::overlay().lowerFd();
  ssize_t n = ::readlinkat(base, rel.c_str(), buf, size - 1);
  if (n < 0)
    return errnoErr();
  buf[n] = '\0';
  return 0;
}

int impl_mknod(const std::string &rel, mode_t mode, dev_t rdev) {
  if (rel.empty())
    return -EEXIST;
  Ops::overlay().removeWhiteout(rel);
  std::string err;
  if (!Ops::overlay().ensureUpperParent(rel, err))
    return -EIO;
  if (::mknodat(Ops::overlay().upperFd(), rel.c_str(), mode, rdev) != 0)
    return errnoErr();
  return 0;
}

int impl_mkdir(const std::string &rel, mode_t mode) {
  if (rel.empty())
    return -EEXIST;
  Ops::overlay().removeWhiteout(rel);
  std::string err;
  if (!Ops::overlay().ensureUpperParent(rel, err))
    return -EIO;
  if (::mkdirat(Ops::overlay().upperFd(), rel.c_str(), mode) != 0)
    return errnoErr();
  ::fchmodat(Ops::overlay().upperFd(), rel.c_str(), mode & 07777, 0);
  // 与 fuse-overlayfs 行为对齐：挂载态新建目录必带 opaque 标记
  // （ogit 的 upper-only status 依赖该标记把新目录推断为可提交条目）
  if (!Ops::overlay().markOpaque(rel, err))
    return -EIO;
  return 0;
}

int impl_unlink(const std::string &rel) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  if (S_ISDIR(st.st_mode))
    return -EISDIR;
  if (l == Layer::Upper) {
    if (::unlinkat(Ops::overlay().upperFd(), rel.c_str(), 0) != 0)
      return errnoErr();
  }
  if (Ops::overlay().lowerExists(rel)) { // 遮住 lower → 留白名单
    std::string err;
    if (!Ops::overlay().makeWhiteout(rel, err)) {
      std::fprintf(stderr, "[fuse] whiteout(%s): %s\n", rel.c_str(),
                   err.c_str());
      return -EIO;
    }
  }
  return 0;
}

int impl_rmdir(const std::string &rel) {
  if (rel.empty())
    return -EBUSY;
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  if (!S_ISDIR(st.st_mode))
    return -ENOTDIR;
  if (l == Layer::Upper) {
    // merged 空目录的 upper 副本可能只剩 whiteout/标记 → 递归清掉
    if (!removeEntryAt(Ops::overlay().upperFd(), rel, true))
      return errnoErr();
  }
  if (Ops::overlay().lowerExists(rel)) {
    std::string err;
    if (!Ops::overlay().makeWhiteout(rel, err)) {
      std::fprintf(stderr, "[fuse] whiteout(%s): %s\n", rel.c_str(),
                   err.c_str());
      return -EIO;
    }
  }
  return 0;
}

int impl_symlink(const std::string &target, const std::string &rel) {
  if (rel.empty())
    return -EEXIST;
  Ops::overlay().removeWhiteout(rel);
  std::string err;
  if (!Ops::overlay().ensureUpperParent(rel, err))
    return -EIO;
  if (::symlinkat(target.c_str(), Ops::overlay().upperFd(), rel.c_str()) != 0)
    return errnoErr();
  return 0;
}

int impl_rename(const std::string &from, const std::string &to,
                unsigned int flags) {
  if (flags != 0)
    return -EINVAL; // 不支持 RENAME_EXCHANGE / RENAME_NOREPLACE
  struct stat fst{};
  Layer lf = Ops::overlay().resolve(from, &fst);
  if (lf == Layer::None)
    return -ENOENT;
  if (from == to)
    return 0;

  // 无 redirect_dir 语义：lower 侧存在的目录不允许改名（与内核 overlayfs
  // 一致返回 EXDEV）
  if (S_ISDIR(fst.st_mode) && Ops::overlay().lowerExists(from))
    return -EXDEV;

  int rc = copyUpIfNeeded(from, lf); // 文件 rename 需要 upper 副本
  if (rc != 0)
    return rc;

  struct stat tst{};
  Layer lt = Ops::overlay().resolve(to, &tst);
  if (lt != Layer::None) {
    if (S_ISDIR(fst.st_mode) != S_ISDIR(tst.st_mode)) {
      return S_ISDIR(fst.st_mode) ? -ENOTDIR : -EISDIR;
    }
    if (lt == Layer::Upper) {
      if (!removeEntryAt(Ops::overlay().upperFd(), to, S_ISDIR(tst.st_mode)))
        return errnoErr();
    }
    // lower 侧旧目标被新 upper 条目直接遮住，无需白名单
  }
  std::string err;
  if (!Ops::overlay().ensureUpperParent(to, err))
    return -EIO;
  if (::renameat(Ops::overlay().upperFd(), from.c_str(),
                 Ops::overlay().upperFd(), to.c_str()) != 0)
    return errnoErr();
  // 旧路径若在 lower 有副本 → 白名单遮住（否则合并视图里旧文件会"复活"）
  if (Ops::overlay().lowerExists(from)) {
    if (!Ops::overlay().makeWhiteout(from, err)) {
      std::fprintf(stderr, "[fuse] whiteout(%s): %s\n", from.c_str(),
                   err.c_str());
      return -EIO;
    }
  }
  return 0;
}

int impl_link(const std::string &from, const std::string &to) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(from, &st);
  if (l == Layer::None)
    return -ENOENT;
  if (S_ISDIR(st.st_mode))
    return -EPERM;
  int rc = copyUpIfNeeded(from, l);
  if (rc != 0)
    return rc;
  Ops::overlay().removeWhiteout(to);
  std::string err;
  if (!Ops::overlay().ensureUpperParent(to, err))
    return -EIO;
  if (::linkat(Ops::overlay().upperFd(), from.c_str(), Ops::overlay().upperFd(),
               to.c_str(), 0) != 0)
    return errnoErr();
  return 0;
}

int impl_chmod(const std::string &rel, mode_t mode, struct fuse_file_info *fi) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  if (fi && fi->fh) {
    if (::fchmod(static_cast<int>(fi->fh & 0xFFFFFFFF), mode) != 0)
      return errnoErr();
    return 0;
  }
  int rc = copyUpIfNeeded(rel, l);
  if (rc != 0)
    return rc;
  if (::fchmodat(Ops::overlay().upperFd(), rel.c_str(), mode, 0) != 0)
    return errnoErr();
  return 0;
}

int impl_chown(const std::string &rel, uid_t uid, gid_t gid,
               struct fuse_file_info *fi) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  if (fi && fi->fh) {
    if (::fchown(static_cast<int>(fi->fh & 0xFFFFFFFF), uid, gid) != 0)
      return errnoErr();
    return 0;
  }
  int rc = copyUpIfNeeded(rel, l);
  if (rc != 0)
    return rc;
  if (::fchownat(Ops::overlay().upperFd(), rel.c_str(), uid, gid,
                 AT_SYMLINK_NOFOLLOW) != 0) {
    return errnoErr();
  }
  return 0;
}

int impl_truncate(const std::string &rel, off_t size,
                  struct fuse_file_info *fi) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  if (fi && fi->fh) {
    if (::ftruncate(static_cast<int>(fi->fh & 0xFFFFFFFF), size) != 0)
      return errnoErr();
    return 0;
  }
  int rc = copyUpIfNeeded(rel, l);
  if (rc != 0)
    return rc;
  int fd = ::openat(Ops::overlay().upperFd(), rel.c_str(), O_WRONLY);
  if (fd < 0)
    return errnoErr();
  int trc = ::ftruncate(fd, size);
  ::close(fd);
  return trc == 0 ? 0 : errnoErr();
}

int impl_utimens(const std::string &rel, const struct timespec tv[2],
                 struct fuse_file_info *fi) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  if (fi && fi->fh) {
    if (::futimens(static_cast<int>(fi->fh & 0xFFFFFFFF), tv) != 0)
      return errnoErr();
    return 0;
  }
  int rc = copyUpIfNeeded(rel, l);
  if (rc != 0)
    return rc;
  if (::utimensat(Ops::overlay().upperFd(), rel.c_str(), tv,
                  AT_SYMLINK_NOFOLLOW) != 0)
    return errnoErr();
  return 0;
}

int impl_create(const std::string &rel, mode_t mode,
                struct fuse_file_info *fi) {
  if (rel.empty())
    return -EEXIST;
  Ops::overlay().removeWhiteout(rel);
  std::string err;
  if (!Ops::overlay().ensureUpperParent(rel, err))
    return -EIO;
  int fd =
      ::openat(Ops::overlay().upperFd(), rel.c_str(), fi->flags | O_EXCL, mode);
  if (fd < 0)
    return errnoErr();
  fi->fh =
      static_cast<uint64_t>(fd) | ((fi->flags & O_APPEND) ? kFhAppendBit : 0);
  ++g_openCount;
  return 0;
}

int impl_open(const std::string &rel, struct fuse_file_info *fi) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  int accmode = fi->flags & O_ACCMODE;
  bool wantWrite = accmode != O_RDONLY || (fi->flags & O_TRUNC);
  if (wantWrite && l == Layer::Lower) {
    int rc = copyUpIfNeeded(rel, l); // 写打开 / 截断 → copy-up
    if (rc != 0)
      return rc;
    l = Layer::Upper;
  }
  int base =
      l == Layer::Upper ? Ops::overlay().upperFd() : Ops::overlay().lowerFd();
  int flags = fi->flags & ~(O_EXCL); // open 已过原子创建阶段
  int fd = ::openat(base, rel.empty() ? "." : rel.c_str(), flags);
  if (fd < 0)
    return errnoErr();
  fi->fh =
      static_cast<uint64_t>(fd) | ((fi->flags & O_APPEND) ? kFhAppendBit : 0);
  ++g_openCount;
  return 0;
}

int impl_read(const std::string &, char *buf, size_t size, off_t off,
              struct fuse_file_info *fi) {
  int fd = static_cast<int>(fi->fh & 0xFFFFFFFF);
  ssize_t n;
  do {
    n = ::pread(fd, buf, size, off);
  } while (n < 0 && errno == EINTR);
  if (n < 0)
    return errnoErr();
  return static_cast<int>(n);
}

int impl_write(const std::string &, const char *buf, size_t size, off_t off,
               struct fuse_file_info *fi) {
  int fd = static_cast<int>(fi->fh & 0xFFFFFFFF);
  ssize_t n;
  if (fi->fh & kFhAppendBit) {
    // O_APPEND 语义：忽略内核给出的偏移，强制写到文件尾
    do {
      n = ::write(fd, buf, size);
    } while (n < 0 && errno == EINTR);
  } else {
    do {
      n = ::pwrite(fd, buf, size, off);
    } while (n < 0 && errno == EINTR);
  }
  if (n < 0)
    return errnoErr();
  return static_cast<int>(n);
}

int impl_statfs(const std::string &, struct statvfs *st) {
  // 以 upper 的容量信息为准（新写入都落在 upper）
  if (::fstatvfs(Ops::overlay().upperFd(), st) != 0)
    return errnoErr();
  return 0;
}

int impl_flush(const std::string &, struct fuse_file_info *) { return 0; }

int impl_release(const std::string &, struct fuse_file_info *fi) {
  if (fi->fh) {
    int fd = static_cast<int>(fi->fh & 0xFFFFFFFF);
    if (::close(fd) == 0 && g_openCount > 0)
      --g_openCount;
    fi->fh = 0;
  }
  return 0;
}

int impl_fsync(const std::string &, int datasync, struct fuse_file_info *fi) {
  int fd = static_cast<int>(fi->fh & 0xFFFFFFFF);
  int rc = datasync ? ::fdatasync(fd) : ::fsync(fd);
  return rc == 0 ? 0 : errnoErr();
}

int impl_opendir(const std::string &rel, struct fuse_file_info *fi) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  if (!S_ISDIR(st.st_mode))
    return -ENOTDIR;
  int base =
      l == Layer::Upper ? Ops::overlay().upperFd() : Ops::overlay().lowerFd();
  int fd =
      ::openat(base, rel.empty() ? "." : rel.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    return errnoErr();
  fi->fh = static_cast<uint64_t>(fd);
  return 0;
}

int impl_readdir(const std::string &rel, void *buf, fuse_fill_dir_t filler,
                 off_t off, struct fuse_file_info *,
                 enum fuse_readdir_flags flags) {
  auto entries = Ops::overlay().mergedList(rel);
  if (off < 0)
    return -EINVAL;
  bool plus = (flags & FUSE_READDIR_PLUS) != 0;
  for (size_t i = static_cast<size_t>(off); i < entries.size(); ++i) {
    const auto &e = entries[i];
    std::string child = rel.empty() ? e.name : rel + "/" + e.name;
    struct stat st{};
    if (e.layer == Layer::Upper)
      st = Ops::overlay().upperStat(child).value_or(st);
    else
      st = Ops::overlay().lowerStat(child).value_or(st);
    if (filler(buf, e.name.c_str(), &st, static_cast<off_t>(i + 1),
               plus ? FUSE_FILL_DIR_PLUS
                    : static_cast<fuse_fill_dir_flags>(0)) != 0) {
      break; // 缓冲区满，内核会带着 off 续传
    }
  }
  return 0;
}

int impl_releasedir(const std::string &, struct fuse_file_info *fi) {
  if (fi->fh) {
    ::close(static_cast<int>(fi->fh & 0xFFFFFFFF));
    fi->fh = 0;
  }
  return 0;
}

int impl_fsyncdir(const std::string &, int datasync,
                  struct fuse_file_info *fi) {
  int fd = static_cast<int>(fi->fh & 0xFFFFFFFF);
  int rc = datasync ? ::fdatasync(fd) : ::fsync(fd);
  return rc == 0 ? 0 : errnoErr();
}

int impl_access(const std::string &rel, int mask) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  int base =
      l == Layer::Upper ? Ops::overlay().upperFd() : Ops::overlay().lowerFd();
  if (::faccessat(base, rel.empty() ? "." : rel.c_str(), mask, 0) != 0)
    return errnoErr();
  return 0;
}

int impl_setxattr(const std::string &rel, const char *name, const char *value,
                  size_t size, int xflags) {
  if (isInternalXattr(name))
    return -EPERM; // overlay 元数据不对用户开放写
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  int rc = copyUpIfNeeded(rel, l);
  if (rc != 0)
    return rc;
  int fd = ::openat(Ops::overlay().upperFd(), rel.c_str(), O_PATH | O_NOFOLLOW);
  if (fd < 0)
    return errnoErr();
  int src = ::fsetxattr(fd, name, value, size, xflags);
  ::close(fd);
  return src == 0 ? 0 : errnoErr();
}

int impl_getxattr(const std::string &rel, const char *name, char *value,
                  size_t size) {
  if (isInternalXattr(name))
    return -ENODATA; // 隐藏内部元数据
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  int base =
      l == Layer::Upper ? Ops::overlay().upperFd() : Ops::overlay().lowerFd();
  int fd = ::openat(base, rel.empty() ? "." : rel.c_str(), O_PATH | O_NOFOLLOW);
  if (fd < 0)
    return errnoErr();
  ssize_t n = ::fgetxattr(fd, name, value, size);
  ::close(fd);
  if (n < 0)
    return errnoErr();
  return static_cast<int>(n);
}

int impl_listxattr(const std::string &rel, char *list, size_t size) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  int base =
      l == Layer::Upper ? Ops::overlay().upperFd() : Ops::overlay().lowerFd();
  auto all = listXattrsAt(base, rel);
  std::vector<std::string> visible;
  size_t need = 0;
  for (const auto &x : all) {
    if (isInternalXattr(x.c_str()))
      continue;
    visible.push_back(x);
    need += x.size() + 1;
  }
  if (size == 0)
    return static_cast<int>(need);
  if (size < need)
    return -ERANGE;
  size_t off = 0;
  for (const auto &x : visible) {
    std::memcpy(list + off, x.c_str(), x.size() + 1);
    off += x.size() + 1;
  }
  return static_cast<int>(need);
}

int impl_removexattr(const std::string &rel, const char *name) {
  if (isInternalXattr(name))
    return -EPERM;
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  int rc = copyUpIfNeeded(rel, l);
  if (rc != 0)
    return rc;
  int fd = ::openat(Ops::overlay().upperFd(), rel.c_str(), O_PATH | O_NOFOLLOW);
  if (fd < 0)
    return errnoErr();
  int rrc = ::fremovexattr(fd, name);
  ::close(fd);
  return rrc == 0 ? 0 : errnoErr();
}

int impl_fallocate(const std::string &rel, int mode, off_t off, off_t len,
                   struct fuse_file_info *fi) {
  struct stat st{};
  Layer l = Ops::overlay().resolve(rel, &st);
  if (l == Layer::None)
    return -ENOENT;
  int fd = -1;
  bool opened = false;
  if (fi && fi->fh) {
    fd = static_cast<int>(fi->fh & 0xFFFFFFFF);
  } else {
    int rc = copyUpIfNeeded(rel, l);
    if (rc != 0)
      return rc;
    fd = ::openat(Ops::overlay().upperFd(), rel.c_str(), O_WRONLY);
    if (fd < 0)
      return errnoErr();
    opened = true;
  }
  int rc = ::fallocate(fd, mode, off, len);
  if (opened)
    ::close(fd);
  return rc == 0 ? 0 : errnoErr();
}

void *impl_init(struct fuse_conn_info *, struct fuse_config *cfg) {
  // 缓存策略（挂载态提交的可见性基石）：
  //   0 超时 =
  //   内核永不信任缓存的属性/条目/负查询，每次都回到守护进程拿真实状态；
  //   页缓存在 open 时自动失效（未启用 kernel_cache）。
  // 提交后另有 fuse_invalidate_path 主动失效作为双保险。
  cfg->entry_timeout = 0;
  cfg->negative_timeout = 0;
  cfg->attr_timeout = 0;
  cfg->kernel_cache = 0;
#if 0
  std::printf("[fuse] init: lower=%s upper=%s mount=%s\n",
              Ops::overlay().cfg.mount.c_str(), Ops::overlay().cfg.upper.c_str(),
              Ops::overlay().cfg.mount.c_str());
#endif
  return nullptr;
}

void impl_destroy(void *) { std::printf("[fuse] destroy\n"); }

} // namespace

// ---------- ops 表 ----------
template <auto F> struct OPT_IMPL_HELPER;
template <int (*F)(const std::string &, const std::string &)>
struct OPT_IMPL_HELPER<F> {
  template <auto T> static int warp(const char *tar, const char *p);

  template <std::mutex &t> static int warp(const char *tar, const char *p) {
    std::lock_guard<std::mutex> lk(t);
    return F(tar, relOf(p));
  }
};
template <typename... Rest, int (*F)(const std::string &, Rest...)>
struct OPT_IMPL_HELPER<F> {
  template <auto T> static int warp(const char *p, Rest... rest);

  static int warp(const char *p, Rest... rest) { return F(relOf(p), rest...); }

  template <std::mutex &t> static int warp(const char *p, Rest... rest) {
    std::lock_guard<std::mutex> lk(t);
    return F(relOf(p), rest...);
  }
};

template <auto F> struct OPT_IMPL_HELPER_DUAL;
template <typename... Rest,
          int (*F)(const std::string &, const std::string &, Rest...)>
struct OPT_IMPL_HELPER_DUAL<F> {
  template <auto T> static int warp(const char *a, const char *b, Rest... rest);

  template <std::mutex &t>
  static int warp(const char *a, const char *b, Rest... rest) {
    std::lock_guard<std::mutex> lk(t);
    return F(relOf(a), relOf(b), rest...);
  }
};

fuse_operations Ops::buildOps() {
  fuse_operations ops{};
  ops.getattr = OPT_IMPL_HELPER<impl_getattr>::warp<g_stateMutex>;
  ops.readlink = OPT_IMPL_HELPER<impl_readlink>::warp<g_stateMutex>;
  ops.mknod = OPT_IMPL_HELPER<impl_mknod>::warp<g_stateMutex>;
  ops.mkdir = OPT_IMPL_HELPER<impl_mkdir>::warp<g_stateMutex>;
  ops.unlink = OPT_IMPL_HELPER<impl_unlink>::warp<g_stateMutex>;
  ops.rmdir = OPT_IMPL_HELPER<impl_rmdir>::warp<g_stateMutex>;
  ops.symlink = OPT_IMPL_HELPER<impl_symlink>::warp<g_stateMutex>;
  ops.rename = OPT_IMPL_HELPER_DUAL<impl_rename>::warp<g_stateMutex>;
  ops.link = OPT_IMPL_HELPER_DUAL<impl_link>::warp<g_stateMutex>;
  ops.chmod = OPT_IMPL_HELPER<impl_chmod>::warp<g_stateMutex>;
  ops.chown = OPT_IMPL_HELPER<impl_chown>::warp<g_stateMutex>;
  ops.truncate = OPT_IMPL_HELPER<impl_truncate>::warp<g_stateMutex>;
  ops.utimens = OPT_IMPL_HELPER<impl_utimens>::warp<g_stateMutex>;
  ops.create = OPT_IMPL_HELPER<impl_create>::warp<g_stateMutex>;
  ops.open = OPT_IMPL_HELPER<impl_open>::warp<g_stateMutex>;
  ops.read = OPT_IMPL_HELPER<impl_read>::warp;
  ops.write = OPT_IMPL_HELPER<impl_write>::warp;
  ops.statfs = OPT_IMPL_HELPER<impl_statfs>::warp<g_stateMutex>;
  ops.flush = OPT_IMPL_HELPER<impl_flush>::warp;
  ops.release = OPT_IMPL_HELPER<impl_release>::warp;
  ops.fsync = OPT_IMPL_HELPER<impl_fsync>::warp;
  ops.opendir = OPT_IMPL_HELPER<impl_opendir>::warp<g_stateMutex>;
  ops.readdir = OPT_IMPL_HELPER<impl_readdir>::warp<g_stateMutex>;
  ops.releasedir = OPT_IMPL_HELPER<impl_releasedir>::warp;
  ops.fsyncdir = OPT_IMPL_HELPER<impl_fsyncdir>::warp;
  ops.access = OPT_IMPL_HELPER<impl_access>::warp<g_stateMutex>;
  ops.init = impl_init;
  ops.destroy = impl_destroy;
  ops.fallocate = OPT_IMPL_HELPER<impl_fallocate>::warp<g_stateMutex>;
  ops.setxattr = OPT_IMPL_HELPER<impl_setxattr>::warp<g_stateMutex>;
  ops.getxattr = OPT_IMPL_HELPER<impl_getxattr>::warp<g_stateMutex>;
  ops.listxattr = OPT_IMPL_HELPER<impl_listxattr>::warp<g_stateMutex>;
  ops.removexattr = OPT_IMPL_HELPER<impl_removexattr>::warp<g_stateMutex>;
  return ops;
}

} // namespace alluvial_fuse

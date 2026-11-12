// overlay.hpp — fuse 覆盖层模型：lower/upper 双层解析、白名单、opaque、copy-up
//
// 兼容 containers/fuse-overlayfs 的磁盘格式约定（与 pi-agent/ogit
// 的识别逻辑一致）：
//   - 白名单（whiteout）：字符设备 0:0；mknod 不可用时回退为普通文件 .wh.<name>
//   - opaque 目录：目录内标记文件 .wh..wh..opq，和/或 xattr
//   user.fuseoverlayfs.opaque="y"
//   - copy-up 来源：regular file 上 xattr user.fuseoverlayfs.origin = 条目 rel
//   路径
//
// 访问模型：init() 在挂载【前】预打开 lower/upper 的目录 fd；此后全部经 *at()
// 相对 fd 访问（见 atfs.hpp）。lower
// 同时是挂载点时，按路径访问会在挂载后穿过自身 FUSE
// 挂载导致守护进程自等待死锁——fd 相对访问是 fuse-overlayfs 的同款解法。
#pragma once

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>
#include <wordexp.h>

#include "id.hpp"

#include <sys/stat.h>

namespace alluvial_fuse {

namespace fs = std::filesystem;
using namespace std;

// 磁盘格式常量（与 fuse-overlayfs 保持一致）
constexpr const char *kOpaqueMarker = ".wh..wh..opq";
constexpr const char *kWhiteoutPrefix = ".wh.";
constexpr const char *kOriginXattr = "user.fuseoverlayfs.origin";
constexpr const char *kOriginXattrPriv = "trusted.overlay.origin";
constexpr const char *kOpaqueXattr = "user.fuseoverlayfs.opaque";
constexpr const char *kOpaqueXattrPriv = "trusted.overlay.opaque";

inline const char *kOpaqueMarkerName() { return kOpaqueMarker; }
inline const char *kWhiteoutPrefixName() { return kWhiteoutPrefix; }

// 合并解析结果：条目位于哪一层
enum class Layer {
  None,  // 不存在（含被白名单隐藏）
  Lower, // 仅 lower（读侧直接用；写侧需 copy-up）
  Upper, // upper 有副本（upper 优先）
};

struct OverlayConfig {
  fs::path mount; // lower is mount.
  fs::path upper;
  fs::path work; // FIXME: 预留（对齐 fuse-overlayfs 参数；内部未使用）

  void weakly_canonical() {
    constexpr const char *default_path = "~/.local/share/{}/{}";
    auto expand_tilde = [](const string path) -> string {
      wordexp_t exp_result;
      // 使用 WRDE_NOCMD 标志禁止执行命令替换，保障安全
      if (wordexp(path.c_str(), &exp_result, WRDE_NOCMD) == 0) {
        string expanded(exp_result.we_wordv[0]);
        wordfree(&exp_result);
        return expanded;
      }
      // 如果解析失败，返回原路径
      return path;
    };
    mount = fs::weakly_canonical(mount);
    auto name =
        to_string(hash<string>{}(mount)) + "_" + mount.filename().string();

    upper = upper.empty() ? fs::weakly_canonical(fs::path(expand_tilde(format(
                                default_path, id.filename(), name + "_upper"))))
                          : fs::weakly_canonical(upper);
    work = work.empty() ? fs::weakly_canonical(fs::path(expand_tilde(format(
                              default_path, id.filename(), name + "_work"))))
                        : fs::weakly_canonical(work);
  }
};

class Overlay {
public:
  /**
   * @note 校验目录并预打开 lower/upper 目录 fd —— 必须发生在挂载之前：
   *        lower 与挂载点相同时，挂载后按路径访问会穿过自身 FUSE 挂载而死锁
   */
  void init(OverlayConfig &&cfg);

  int lowerFd() const { return lowerFd_; }
  int upperFd() const { return upperFd_; }

  const OverlayConfig &config() const { return cfg; }

  std::optional<struct stat> lowerStat(const std::string &rel) const;
  std::optional<struct stat> upperStat(const std::string &rel) const;
  bool lowerExists(const std::string &rel) const;
  bool upperExists(const std::string &rel) const;

  // upper 中该条目是否为白名单（字符 0:0 或 .wh.<name> 回退文件）。
  // 注意 .wh..wh..opq 是 opaque 标记，不算白名单。
  bool isWhiteout(const std::string &rel) const;
  // upper 目录是否为 opaque（xattr 或标记文件任一成立）
  bool isOpaqueDir(const std::string &rel) const;

  // 合并视图解析：白名单 → None；upper 优先；其次 lower
  Layer resolve(const std::string &rel, struct stat *outSt = nullptr) const;

  // readdir 合并：upper ∪ lower，跳过白名单与 opaque 标记，opaque 时不含 lower
  // 条目。
  struct Entry {
    std::string name;
    Layer layer;
  };
  std::vector<Entry> mergedList(const std::string &rel) const;

  // ---------- 变更类操作（仅由 FUSE ops 与 commit 流程调用） ----------

  // 确保 upper 中父目录链存在（普通目录，不带 opaque）
  bool ensureUpperParent(const std::string &rel, std::string &err) const;
  // copy-up：把 lower 条目复制到 upper（regular file 带 origin xattr）
  bool copyUp(const std::string &rel, std::string &err) const;
  // 在 upper 创建白名单（优先字符设备 0:0，mknod 失败回退 .wh.<name> 文件）
  bool makeWhiteout(const std::string &rel, std::string &err) const;
  // 移除该路径上的白名单（两种格式都尝试，尽力而为）
  void removeWhiteout(const std::string &rel) const;
  // 标记/清除 opaque（标记文件 + xattr 双写 / 双清）
  bool markOpaque(const std::string &rel, std::string &err) const;
  void clearOpaque(const std::string &rel) const;

private:
  OverlayConfig cfg;
  int lowerFd_ = -1;
  int upperFd_ = -1;
};

} // namespace alluvial_fuse

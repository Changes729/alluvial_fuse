// util.hpp — fuse 基础工具：路径、lstat、xattr、目录枚举
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

namespace alluvial_fuse {

// ---------- 路径 ----------
std::string absPath(const std::string& p);
std::string joinPath(const std::string& a, const std::string& b);
// rel 是否位于 prefix 目录之下（或与其相等）
bool pathUnder(const std::string& rel, const std::string& prefix);

// ---------- 文件 ----------
std::optional<struct stat> lstatOpt(const std::string& path);
// overlay 白名单：字符设备 0:0（fuse-overlayfs / 内核 overlayfs 约定）
bool isCharWhiteout(const struct stat& st);
// 覆盖层条目是否为白名单（字符设备 0:0，或 mknod 失败时的 .wh.<name> 回退文件）。
// opaque 标记文件 .wh..wh..opq 不属于白名单（由调用方单独跳过）。
bool isWhiteoutEntry(const std::string& name, const struct stat& st);
// .wh.<name> 回退白名单对应的真实条目名（非回退格式返回原名）
std::string whiteoutTargetName(const std::string& name);
bool hasXattrValue(const std::string& path, const char* name);  // xattr 存在即真
bool readXattrValue(const std::string& path, const char* name, std::string& out);
std::vector<std::string> listXattrs(const std::string& path);
std::vector<std::string> listDir(const std::string& dir);  // 已排序，不含 . / ..
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& content);
std::string readSymlink(const std::string& path);
bool mkdirs(const std::string& path, mode_t mode = 0755);
bool removeEntry(const std::string& path, bool isDir);  // 文件 unlink / 目录递归删
bool parentDirOf(const std::string& path, std::string& out);

// ---------- 进程 ----------
// 在 PATH 与给定的候选绝对路径中查找可执行文件；找不到返回 ""
std::string findExecutable(const std::string& name, const std::vector<std::string>& extraPaths);

// ---------- /proc/mounts ----------
std::string unescapeMount(const std::string& s);
// 挂载点当前是否被 fuse / overlay 类型挂载覆盖
bool isMounted(const std::string& mountPoint);

// ---------- 其他 ----------
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
std::string basenameOf(const std::string& path);

}  // namespace fuse

// atfs.hpp — 基于 dirfd 的文件操作层
//
// 守护进程把 lower(A/) 同时作为挂载点：挂载完成后按路径访问 lower 会穿过自己的
// FUSE 挂载 → 守护进程等待自己答复 → 死锁（request_wait_answer）。
// 与 fuse-overlayfs 同解：挂载前预打开 lower/upper 目录 fd，之后一切访问都用
// openat/fstatat/unlinkat 等 *at() 相对 fd 完成，永不经过挂载点路径。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace alluvial_fuse {

// 相对 rel 的查询/读写（rel == "" 表示 dirfd 本身）
std::optional<struct stat> lstatAt(int dirfd, const std::string& rel);
bool existsAt(int dirfd, const std::string& rel);
// 目录枚举（已排序，不含 . / ..；目录不存在 → 空列表）
std::vector<std::string> listDirAt(int dirfd, const std::string& rel);
bool readFileAt(int dirfd, const std::string& rel, std::string& out);
std::string readSymlinkAt(int dirfd, const std::string& rel);
// 递归建目录（已存在视为成功）
bool mkdirsAt(int dirfd, const std::string& rel, mode_t mode = 0755);
// 递归删除（文件 unlinkat / 目录 rmdirat 自底向上）
bool removeEntryAt(int dirfd, const std::string& rel, bool isDir);

// xattr（经 O_PATH fd，支持符号链接不加 follow）
bool hasXattrAt(int dirfd, const std::string& rel, const char* name);
bool readXattrAt(int dirfd, const std::string& rel, const char* name, std::string& out);
bool writeXattrAt(int dirfd, const std::string& rel, const char* name, const std::string& value);
bool removeXattrAt(int dirfd, const std::string& rel, const char* name);
std::vector<std::string> listXattrsAt(int dirfd, const std::string& rel);

// O_PATH 打开（失败 -1）
int openPathAt(int dirfd, const std::string& rel);

}  // namespace fuse

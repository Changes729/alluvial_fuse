# Alluvial FUSE

[简体中文](#简介) | [English](#overview)

用户态 overlay 文件系统，基于 FUSE（libfuse3）实现。

---

<a id="简介"></a>

## 简介

Alluvial FUSE 实现了一个**用户态 overlay file system**：将 lower 层（只读基础目录）与 upper 层（可写覆盖目录）合并为统一的挂载视图，所有写入均落入 upper 层，lower 层保持原样不被修改。

与普通 overlay 方案不同，**挂载目录本身就是 lower 目录**——这是本项目的创新点：用户只能通过 overlay 视图访问 lower 内容，写入自动重定向到 upper 层，从而防止用户随意修改 lower 的内容。

## 特性

- 纯用户态实现（FUSE3），无需内核 overlay 驱动
- 挂载目录即 lower 目录，天然保护 lower 层不被直接改写
- copy-up 语义：修改 lower 文件时自动复制到 upper 再写入
- upper / work 目录缺省自动创建，开箱即用
- 支持透传 libfuse 挂载参数

## 快速开始

### 依赖

- C++17 及以上编译器（推荐 C++20）
- CMake >= 4.0
- libfuse3（`pkg-config` 可检索到 `fuse3`）
- pkg-config

### 构建

```bash
./script/build.sh
# 等价于：
# cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build
```

构建产物位于 `build/bin/alluvial_fuse`。

### 基本使用

```bash
alluvial_fuse --mount [mount/point]
```

仅传 `--mount` 时，upper 目录与工作目录会在 `~/.local/share/alluvial_fuse/xxx` 下自动创建。

### 命令行参数

| 参数 | 说明 |
| --- | --- |
| `--mount [mount/point]` | 指定挂载点（同时即 lower 层目录） |
| `--lower [lower/dir]` | 指定 lower 层目录 |
| `--upper [upper/dir]` | 指定 upper 层目录 |
| `--workdir [work/dir]` | 指定工作目录 |
| `-o [opts]` | 透传 libfuse 挂载参数 |

### 测试

```bash
./test/test.sh                  # 全部测试点，按序执行
./test/test.sh overlay          # 只跑 Overlay 覆盖语义测试
./test/test.sh mount perf       # 只跑挂载测试与性能测试
./test/test.sh default          # 只跑默认目录测试
```

测试覆盖：FUSE 挂载与退出、基本 Overlay 覆盖语义（lower/upper 合并视图、文件删除白名单）、新增/删除/更新文件性能对比、默认目录行为。详见 [`test/README.md`](test/README.md)。

## 代码结构

- `src/`: 源代码
  - `main.cpp`: 入口与命令行解析
  - `overlay/`: overlay 核心实现（双层解析、白名单、opaque 目录、copy-up）
- `test/`: AI 生成的测试用例，主要测试内容参考 [`test/README.md`](test/README.md)
- `script/`: 构建脚本
- `doc/`: 文档与更新日志

---

<a id="overview"></a>

## Overview (English)

Alluvial FUSE implements a **userspace overlay filesystem** built on FUSE (libfuse3). It merges a lower layer (read-only base directory) with an upper layer (writable overlay directory) into a single mounted view. All writes go into the upper layer, leaving the lower layer untouched.

Unlike a conventional overlay setup, **the mount point itself is the lower directory** — this is the key innovation of the project: users can only access lower content through the overlay view, and writes are automatically redirected to the upper layer, preventing users from modifying the lower content directly.

## Features

- Pure userspace implementation (FUSE3), no kernel overlay driver required
- Mount point doubles as the lower directory, inherently protecting the lower layer from direct modification
- Copy-up semantics: lower files are copied to upper on first modification
- Upper/work directories are created automatically by default — zero configuration
- Pass-through of libfuse mount options

## Quick Start

### Dependencies

- C++17 or newer compiler (C++20 recommended)
- CMake >= 4.0
- libfuse3 (discoverable via `pkg-config` as `fuse3`)
- pkg-config

### Build

```bash
./script/build.sh
# Equivalent to:
# cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build
```

The binary is produced at `build/bin/alluvial_fuse`.

### Basic Usage

```bash
alluvial_fuse --mount [mount/point]
```

When only `--mount` is given, the upper directory and work directory are created automatically under `~/.local/share/alluvial_fuse/xxx`.

### Command-line Options

| Option | Description |
| --- | --- |
| `--mount [mount/point]` | Mount point (also serves as the lower directory) |
| `--lower [lower/dir]` | Lower layer directory |
| `--upper [upper/dir]` | Upper layer directory |
| `--workdir [work/dir]` | Working directory |
| `-o [opts]` | Pass options through to libfuse |

### Tests

```bash
./test/test.sh                  # Run the whole suite in order
./test/test.sh overlay          # Overlay merge semantics only
./test/test.sh mount perf       # Mount and performance tests only
./test/test.sh default          # Default-directory behavior only
```

Coverage: FUSE mount/unmount, basic overlay semantics (merged view, whiteout deletion), create/delete/update performance compared against a non-FUSE baseline, and default-directory behavior. See [`test/README.md`](test/README.md) for details.

## Code Structure

- `src/`: source code
  - `main.cpp`: entry point and CLI parsing
  - `overlay/`: overlay core (layer resolution, whiteouts, opaque dirs, copy-up)
- `test/`: AI-generated test cases; see [`test/README.md`](test/README.md)
- `script/`: build scripts
- `doc/`: documentation and changelogs

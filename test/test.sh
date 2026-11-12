#!/bin/bash
# test/test.sh — 主测试入口：依次遍历运行各测试点脚本
#
#   测试 1  test_mount.sh    用户态 FUSE 挂载：挂载/退出；识别异常退出导致的文件系统不可用
#   测试 2  test_overlay.sh  Overlay 覆盖语义：lower(old/keep/remove) × upper(keep/new + remove 白名单)
#   测试 3  test_perf.sh     性能对比：新增/更新/删除 × PERF_N 个文件，FUSE vs 无 FUSE 基线
#   测试 4  test_default.sh  仅 --mount 时 upper/work 自动默认到 ~/.local 并主动创建
#
# Usage: ./test/test.sh [mount|overlay|perf ...]（默认全部，按注册表顺序执行）
# 环境变量（透传给子脚本）：
#   PERF_N     性能测试文件数（默认 100）
#   OOFS_BIN   被测二进制路径（默认 build/bin/alluvial_fuse）
set -u

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

if [ -t 1 ]; then
  C_G=$'\e[32m'; C_R=$'\e[31m'; C_C=$'\e[36m'; C_N=$'\e[0m'
else
  C_G=""; C_R=""; C_C=""; C_N=""
fi

## 用例注册表：按此顺序依次遍历（脚本名:描述）----------------
SUITE=(
  "test_mount.sh:测试 1  用户态 FUSE 挂载与退出"
  "test_overlay.sh:测试 2  Overlay 覆盖语义"
  "test_perf.sh:测试 3  性能对比"
  "test_default.sh:测试 4  仅 --mount 时 upper/work 默认 ~/.local"
)

usage() {
  echo "Usage: $0 [mount|overlay|perf ...]（默认全部按序执行）"
  echo "  PERF_N=100               性能测试文件数"
  echo "  OOFS_BIN=/path/to/fuse   被测二进制"
}

## 按参数过滤要执行的用例；无参数 = 全部 ---------------------
declare -a RUN=()
if [ $# -eq 0 ]; then
  RUN=("${SUITE[@]}")
else
  for arg in "$@"; do
    found=""
    for t in "${SUITE[@]}"; do
      [ "${t%%.*}.sh" = "test_${arg}.sh" ] && { RUN+=("$t"); found=1; break; }
    done
    [ -z "$found" ] && { echo "未知测试点：$arg"; usage; exit 1; }
  done
fi

## 依次遍历执行 ----------------------------------------------
TOTAL=0
FAILED=0
declare -a VERDICT=()

echo "${C_C}======== fuse 测试套件 ========${C_N}"
for item in "${RUN[@]}"; do
  script=${item%%:*}
  desc=${item#*:}
  TOTAL=$((TOTAL + 1))
  echo
  echo "${C_C}>>> 运行 $desc（$script）${C_N}"
  if bash "$SCRIPT_DIR/$script"; then
    VERDICT+=("${C_G}PASS${C_N}  $desc")
  else
    VERDICT+=("${C_R}FAIL${C_N}  $desc")
    FAILED=$((FAILED + 1))
  fi
done

## 套件汇总 ---------------------------------------------------
echo
echo "${C_C}======== 套件汇总 ========${C_N}"
for v in "${VERDICT[@]}"; do
  echo "  $v"
done
echo
if [ $FAILED -eq 0 ]; then
  echo "${C_G}全部 $TOTAL 个测试点通过${C_N}"
  exit 0
else
  echo "${C_R}$FAILED/$TOTAL 个测试点失败${C_N}"
  exit 1
fi

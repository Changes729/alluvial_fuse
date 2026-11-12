#!/bin/bash
# test/lib.sh — 各测试脚本共享的公共辅助（被 test_*.sh source，不直接执行）
#
# 提供：颜色输出 / PASS-FAIL 计数 / 临时目录与清理 / 挂载辅助 / preflight / summary
# 环境变量：OOFS_BIN  被测二进制路径（默认 build/bin/a.out）
set -u

## 路径（以 source 本库的脚本为准）---------------------------
_caller=${BASH_SOURCE[1]:-$0}
SCRIPT_DIR=$(dirname "$(readlink -f "$_caller")")
MAIN_DIR=$SCRIPT_DIR/..
BIN=${OOFS_BIN:-$MAIN_DIR/${BUILD_DIR:-build}/bin/${OOFS_NAME:-a.out}}
MOUNT_TIMEOUT_S=10
STOP_TIMEOUT_S=5

## 输出工具 --------------------------------------------------
if [ -t 1 ]; then
  C_G=$'\e[32m'; C_R=$'\e[31m'; C_Y=$'\e[33m'; C_C=$'\e[36m'; C_N=$'\e[0m'
else
  C_G=""; C_R=""; C_Y=""; C_C=""; C_N=""
fi
PASS=0
FAIL=0
DPID=""
TMP=$(mktemp -d /tmp/oofs_test.XXXXXX)

ok()   { PASS=$((PASS + 1)); echo "${C_G}[PASS]${C_N} $*"; }
bad()  { FAIL=$((FAIL + 1)); echo "${C_R}[FAIL]${C_N} $*"; }
info() { echo "${C_C}[INFO]${C_N} $*"; }
warn() { echo "${C_Y}[WARN]${C_N} $*"; }
sec()  { echo; echo "${C_C}======== $* ========${C_N}"; }

cleanup() {
  [ -n "$DPID" ] && kill -KILL "$DPID" 2>/dev/null
  for m in "$TMP"/t*/mnt; do
    [ -d "$m" ] && fusermount3 -uz "$m" >/dev/null 2>&1
  done
  sleep 0.3 # 等待 lazy unmount 生效
  rm -rf "$TMP" 2>/dev/null
}
trap cleanup EXIT

## 挂载辅助 --------------------------------------------------
is_mounted() {
  if command -v mountpoint >/dev/null 2>&1; then
    timeout 5 mountpoint -q "$1" 2>/dev/null
  else
    local esc=${1// /\\040}
    grep -qs " ${esc} fuse" /proc/mounts
  fi
}

# 挂载点健康：能否在超时内完成一次目录访问（异常退出后通常报 Transport endpoint）
mount_healthy() {
  timeout 5 ls "$1" >/dev/null 2>&1
}

wait_mounted() {
  local deadline=$((SECONDS + MOUNT_TIMEOUT_S))
  while [ $SECONDS -lt $deadline ]; do
    if is_mounted "$1" && mount_healthy "$1"; then return 0; fi
    sleep 0.1
  done
  return 1
}

start_daemon() { # $1=mnt $2=upper $3=work $4=log
  "$BIN" --mount="$1" --upperdir="$2" --workdir="$3" >"$4" 2>&1 &
  DPID=$!
}

# 优雅退出：SIGTERM → 等待退出。返回 0=正常退出(rc=0) 1=超时(SIGKILL兜底) 2=退出但rc非0
graceful_stop() {
  local pid=$1 i rc
  kill -TERM "$pid" 2>/dev/null
  for i in $(seq 1 $((STOP_TIMEOUT_S * 10))); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null
    return 1
  fi
  wait "$pid" 2>/dev/null
  rc=$?
  [ $rc -eq 0 ] && return 0 || return 2
}

dump_log() {
  [ -f "$1" ] && sed 's/^/       /' "$1" | head -20
}

preflight() {
  command -v fusermount3 >/dev/null 2>&1 ||
    { echo "错误：缺少 fusermount3（请安装 fuse3）"; exit 1; }
  [ -x "$BIN" ] ||
    { echo "错误：未找到被测二进制 $BIN（请先执行 ./script/build.sh）"; exit 1; }
  [ -e /dev/fuse ] || warn "/dev/fuse 不存在，FUSE 挂载可能失败"
  info "被测二进制：$BIN"
  info "工作目录：  $TMP"
}

## 汇总退出：本脚本内所有测试点结束时调用 --------------------
summary() {
  sec "汇总"
  echo "PASS: $PASS  FAIL: $FAIL"
  if [ $FAIL -eq 0 ]; then
    echo "${C_G}全部通过${C_N}"
    exit 0
  else
    echo "${C_R}存在失败项${C_N}"
    exit 1
  fi
}

#!/bin/bash
# test/test_mount.sh — 测试 1：用户态 FUSE 挂载与退出
#
#   挂载/退出；识别异常退出导致的文件系统不可用
#   可独立运行：./test/test_mount.sh
set -u
. "$(dirname "$(readlink -f "$0")")/lib.sh"

test_mount() {
  sec "测试 1：用户态 FUSE 挂载与退出"
  local d=$TMP/t1 rc
  mkdir -p "$d/mnt" "$d/upper" "$d/work"
  echo "mount-marker" >"$d/mnt/marker"

  ## 1a 挂载
  start_daemon "$d/mnt" "$d/upper" "$d/work" "$d/daemon.log"
  if wait_mounted "$d/mnt"; then
    ok "挂载成功（挂载表可见且视图可访问）"
  else
    bad "挂载失败（超时 ${MOUNT_TIMEOUT_S}s），daemon 日志："
    dump_log "$d/daemon.log"
    DPID=""
    return
  fi

  ## 1b 挂载视图读取 lower 既有文件
  if [ "$(cat "$d/mnt/marker" 2>/dev/null)" = "mount-marker" ]; then
    ok "挂载视图可读（lower 既有文件内容一致）"
  else
    bad "挂载视图读取失败或内容不一致"
  fi

  ## 1c 正常退出（SIGTERM → graceful unmount）
  graceful_stop "$DPID"; rc=$?
  DPID=""
  case $rc in
  0) ok "守护进程 SIGTERM 后正常退出 (rc=0)" ;;
  1) bad "守护进程未在 ${STOP_TIMEOUT_S}s 内退出（已 SIGKILL 兜底）" ;;
  2) bad "守护进程退出码非 0 (rc=$rc)" ;;
  esac

  if is_mounted "$d/mnt"; then
    bad "退出后挂载点残留（强制清理）"
    fusermount3 -uz "$d/mnt" >/dev/null 2>&1
  else
    ok "退出后挂载点已卸载"
  fi
  if mount_healthy "$d/mnt"; then
    ok "退出后挂载点路径可用（无文件系统不可用残留）"
  else
    bad "退出后挂载点不可访问（异常残留）"
  fi
  if [ "$(cat "$d/mnt/marker" 2>/dev/null)" = "mount-marker" ]; then
    ok "卸载后 lower 原目录内容完好"
  else
    bad "卸载后 lower 原目录内容异常"
  fi

  ## 1d 异常退出（SIGKILL）→ 必须识别文件系统不可用
  info "模拟异常退出：SIGKILL 守护进程"
  start_daemon "$d/mnt" "$d/upper" "$d/work" "$d/daemon2.log"
  if ! wait_mounted "$d/mnt"; then
    bad "二次挂载失败，跳过异常退出检测"
    DPID=""
    return
  fi
  kill -KILL "$DPID" 2>/dev/null
  DPID=""
  sleep 0.3 # 等内核完成 fuse 中断处理
  if mount_healthy "$d/mnt"; then
    bad "异常退出后未检测到文件系统失效（挂载点仍可访问）"
  else
    local err
    err=$(timeout 5 ls "$d/mnt" 2>&1 | head -1)
    ok "已识别异常退出导致的文件系统不可用（${err##*: }）"
  fi
  is_mounted "$d/mnt" && fusermount3 -uz "$d/mnt" >/dev/null 2>&1
  sleep 0.2
  if is_mounted "$d/mnt"; then
    bad "残留挂载清理失败"
  else
    ok "残留挂载已强制清理（fusermount3 -uz）"
  fi
}

preflight
test_mount
summary

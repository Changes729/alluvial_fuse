#!/bin/bash
# test/test_overlay.sh — 测试 2：基本 Overlay 覆盖语义
#
#   lower(old/keep/remove) × upper(keep/new + remove 白名单)
#   可独立运行：./test/test_overlay.sh
set -u
. "$(dirname "$(readlink -f "$0")")/lib.sh"

test_overlay() {
  sec "测试 2：基本 Overlay 覆盖语义"
  local d=$TMP/t2
  mkdir -p "$d/mnt" "$d/upper" "$d/work"
  ## lower（=挂载点目录）三个文件：old / keep / remove
  echo "lower-old" >"$d/mnt/old"
  echo "lower-keep-old" >"$d/mnt/keep"
  echo "lower-remove" >"$d/mnt/remove"
  ## upper 两个文件：keep（最新） / new；remove 以白名单删除
  echo "upper-keep-new" >"$d/upper/keep"
  echo "upper-new" >"$d/upper/new"
  if ! mknod "$d/upper/remove" c 0 0 2>/dev/null; then
    echo "" >"$d/upper/.wh.remove" # mknod 不可用（非 root）时回退 .wh. 格式
  fi

  start_daemon "$d/mnt" "$d/upper" "$d/work" "$d/daemon.log"
  if ! wait_mounted "$d/mnt"; then
    bad "挂载失败，跳过覆盖测试，daemon 日志："
    dump_log "$d/daemon.log"
    DPID=""
    return
  fi

  [ "$(cat "$d/mnt/old" 2>/dev/null)" = "lower-old" ] &&
    ok "old 存在（来自 lower）" ||
    bad "old 不存在或内容不符"
  [ "$(cat "$d/mnt/keep" 2>/dev/null)" = "upper-keep-new" ] &&
    ok "keep 为最新（upper 覆盖 lower）" ||
    bad "keep 未显示 upper 最新内容"
  [ ! -e "$d/mnt/remove" ] &&
    ok "remove 已消失（upper 白名单遮蔽）" ||
    bad "remove 仍可见"
  [ "$(cat "$d/mnt/new" 2>/dev/null)" = "upper-new" ] &&
    ok "new 存在（仅 upper 新建）" ||
    bad "new 不存在"

  local list
  list=$(ls "$d/mnt" 2>/dev/null | sort | tr '\n' ' ')
  [ "$list" = "keep new old " ] &&
    ok "readdir 合并视图正确（keep new old，无 remove）" ||
    bad "readdir 合并视图不符：[$list]"

  graceful_stop "$DPID"; DPID=""
  if is_mounted "$d/mnt"; then
    warn "未完全卸载，强制清理"
    fusermount3 -uz "$d/mnt" >/dev/null 2>&1
  fi

  ## 卸载后核验 lower 未被写侧污染
  [ -f "$d/mnt/remove" ] && [ "$(cat "$d/mnt/remove" 2>/dev/null)" = "lower-remove" ] &&
    ok "卸载后 lower 的 remove 仍在（写侧未污染 lower）" ||
    bad "lower 的 remove 文件丢失"
  [ "$(cat "$d/mnt/old" 2>/dev/null)" = "lower-old" ] &&
    [ "$(cat "$d/mnt/keep" 2>/dev/null)" = "lower-keep-old" ] &&
    ok "卸载后 lower 的 old/keep 保持原值" ||
    bad "lower 的 old/keep 被修改"
}

preflight
test_overlay
summary

#!/bin/bash
# test/test_default.sh — 测试 4：仅 --mount 一个参数时，upper 自动默认到 ~/.local/share
#
#   缺省路径约定（对应 OverlayConfig::weakly_canonical）：
#     upper = ~/.local/share/<appname>/<hash>_<挂载点名>_upper（程序主动创建）
#     work  = ~/.local/share/<appname>/<hash>_<挂载点名>_work（预留参数，对齐
#             fuse-overlayfs；内部未使用，不要求创建）
#   <appname> 为被测二进制名（id.filename()）；<hash> 为 mount 路径的
#   std::hash<string> 值（测试不计算也不匹配 hash 值，只用正则
#   <数字>_<挂载点名>_upper|_work 校验，不另行校验 upper/work 成对同哈希）。
#   目录链（含 ~/.local/share/<appname>）必须由程序主动创建。
#
#   落盘位置说明：std::filesystem 不展开 ~，缺省路径中的 "~" 是字面量目录名、
#   按进程工作目录解析，即 <cwd>/~/.local/share/...；若实现改为展开 $HOME，则落在
#   $HOME/.local/share/...。两处候选均探测，命中其一即通过。
#
#   可独立运行：./test/test_default.sh
set -u
. "$(dirname "$(readlink -f "$0")")/lib.sh"

test_default() {
  sec "测试 4：仅 --mount 时 upper 默认 ~/.local/share/<appname>/<hash>_<mnt>_upper 并主动创建（work 预留不创建）"
  local d=$TMP/t4 appname
  mkdir -p "$d/mnt"
  echo "lower-marker" >"$d/mnt/marker"

  if [ -z "${HOME:-}" ]; then
    bad "HOME 未设置，无法验证 ~/.local/share 默认目录"
    return
  fi

  appname=$(basename "$BIN")

  ## 两处候选根：<cwd>/~/.local/share/<appname>（std::filesystem 语义）/ $HOME/.local/share/<appname>
  local tilde_dir="$PWD/~"
  local root_cwd="$tilde_dir/.local/share/$appname"
  local root_home="$HOME/.local/share/$appname"

  ## 启动前状态快照（评估"主动创建"的目录链层级 + 精确清理）
  local had_cwd_root=0 had_cwd_local=0 had_tilde=0
  local had_home_root=0 had_home_local=0
  [ -d "$root_cwd" ] && had_cwd_root=1
  [ -d "$tilde_dir/.local/share" ] && had_cwd_local=1
  [ -d "$tilde_dir" ] && had_tilde=1
  [ -d "$root_home" ] && had_home_root=1
  [ -d "$HOME/.local/share" ] && had_home_local=1

  local lbefore=$TMP/t4/before.list lafter=$TMP/t4/after.list
  local new=""

  t4_snapshot() { # $1=输出文件：两处候选根下的条目（<root>/<name> 全路径清单，全局排序供 comm 用）
    : >"$1"
    local r
    for r in "$root_cwd" "$root_home"; do
      if [ -d "$r" ]; then
        ls -A "$r" | sed "s|^|$r/|" >>"$1"
      fi
    done
    sort -o "$1" "$1"
    return 0
  }
  t4_cleanup() { # 只删本次新建条目；回收本次新建的父目录链（rmdir 仅删空目录）
    local e
    for e in $new; do
      [ -n "$e" ] && rm -rf "$e"
    done
    if [ "$had_cwd_root" = 0 ] && [ -d "$root_cwd" ]; then
      rmdir "$root_cwd" 2>/dev/null
      [ "$had_cwd_local" = 0 ] && rmdir "$tilde_dir/.local/share" 2>/dev/null
      [ "$had_tilde" = 0 ] && rmdir "$tilde_dir" 2>/dev/null
    fi
    if [ "$had_home_root" = 0 ] && [ -d "$root_home" ]; then
      rmdir "$root_home" 2>/dev/null
      [ "$had_home_local" = 0 ] && rmdir "$HOME/.local/share" 2>/dev/null
    fi
    return 0
  }

  ## 4a 仅 --mount 启动（不传 upperdir/workdir）
  t4_snapshot "$lbefore"
  "$BIN" --mount="$d/mnt" >"$d/daemon.log" 2>&1 &
  DPID=$!
  if ! wait_mounted "$d/mnt"; then
    bad "仅 --mount 挂载失败（超时 ${MOUNT_TIMEOUT_S}s），daemon 日志："
    dump_log "$d/daemon.log"
    DPID=""
    t4_snapshot "$lafter"
    new=$(comm -13 "$lbefore" "$lafter")
    t4_cleanup
    return
  fi
  ok "仅 --mount 一个参数即可挂载成功"

  ## 4b 默认目录：~/.local/share/<appname>/<hash>_<挂载点名>_upper|_work
  ##   upper 必须由程序主动创建；work 为预留参数（内部未使用），创建与否均不判 FAIL
  ##   选出与命名校验均用正则 <数字>_<挂载点名>_upper|_work；测试不匹配/不比对 hash 值
  t4_snapshot "$lafter"
  local upper work
  new=$(comm -13 "$lbefore" "$lafter")
  upper=$(printf '%s\n' "$new" | grep -E '/[0-9]+_mnt_upper$' | head -1)
  work=$(printf '%s\n' "$new" | grep -E '/[0-9]+_mnt_work$' | head -1)

  [ -n "$upper" ] && [ -d "$upper" ] &&
    ok "upper 默认目录已主动创建且命名符合 <hash>_<挂载点名>_upper：${upper/#$PWD/~}" ||
    bad "未找到 upper 默认目录（应为 ~/.local/share/$appname/<hash>_mnt_upper；本次新条目：${new:-无}）"

  if [ -n "$work" ]; then
    [ -d "$work" ] &&
      info "work 默认目录也已创建（预留参数，当前实现内部未使用）：${work/#$PWD/~}" ||
      warn "work 命名条目存在但不是目录：$work"
  else
    info "work 默认目录未创建（预留参数，内部未使用，符合当前实现）"
  fi

  if [ -n "$upper" ] && [ -n "$work" ] && [ -d "$work" ]; then
    [ "$(dirname "$upper")" = "$(dirname "$work")" ] &&
      ok "upper/work 同在 appname 目录下" ||
      bad "upper/work 不在同一 appname 目录"
  fi
  if [ "$had_cwd_root" = 0 ] && [ -d "$root_cwd" ]; then
    info "落盘位置：<cwd>/~/.local/share（std::filesystem 不展开 ~，~ 为字面量目录）"
  elif [ "$had_home_root" = 0 ] && [ -d "$root_home" ]; then
    info "落盘位置：\$HOME/.local/share（实现做了 ~ → \$HOME 展开）"
  fi
  local ncount
  ncount=$(printf '%s\n' "$new" | grep -c .)
  [ "$ncount" = 1 ] ||
    warn "默认创建的条目数为 $ncount（预期仅 upper 一项；work 为预留参数不创建）"

  ## 4c 默认 upper 生效：视图写入落到默认 upper；读 lower 不触发 copy-up
  if [ -n "$upper" ] && [ -d "$upper" ]; then
    echo "through-default-upper" >"$d/mnt/new-in-upper" &&
      grep -qs "through-default-upper" "$upper/new-in-upper" &&
      ok "写入穿透到默认 upper 目录" ||
      bad "写入未落到默认 upper"
    [ ! -e "$upper/marker" ] &&
      ok "读取 lower 既有文件未触发 copy-up" ||
      bad "lower 既有文件意外出现在默认 upper"
  fi

  ## 4d 显式 upperdir/workdir 时不得触碰 ~/.local/share
  local pid1=$DPID
  mkdir -p "$d/mnt2" "$d/upper2" "$d/work2"
  start_daemon "$d/mnt2" "$d/upper2" "$d/work2" "$d/daemon2.log"
  if wait_mounted "$d/mnt2"; then
    t4_snapshot "$lafter"
    local new2
    new2=$(comm -13 "$lbefore" "$lafter")
    if printf '%s\n' "$new2" | grep -q '_mnt2'; then
      bad "显式 upperdir/workdir 时仍在 ~/.local/share 创建了目录"
    elif [ "$new2" != "$new" ]; then
      bad "显式参数时默认目录区出现了额外新条目"
    else
      ok "显式 upperdir/workdir 时 ~/.local/share 未被触碰"
    fi
  else
    warn "第二次挂载（显式参数）失败，跳过 4d，日志："
    dump_log "$d/daemon2.log"
  fi
  graceful_stop "$DPID"; DPID=""
  is_mounted "$d/mnt2" && fusermount3 -uz "$d/mnt2" >/dev/null 2>&1

  ## 4e 退出与善后
  graceful_stop "$pid1"; DPID=""
  if is_mounted "$d/mnt"; then
    warn "未完全卸载，强制清理"
    fusermount3 -uz "$d/mnt" >/dev/null 2>&1
  fi
  t4_cleanup
}

preflight
test_default
summary

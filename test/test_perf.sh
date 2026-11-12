#!/bin/bash
# test/test_perf.sh — 测试 3：性能对比
#
#   新增/更新/删除 × PERF_N 个文件，FUSE vs 无 FUSE 基线
#   可独立运行：./test/test_perf.sh
#   环境变量：PERF_N 性能测试文件数（默认 100）
set -u
. "$(dirname "$(readlink -f "$0")")/lib.sh"

PERF_N=${PERF_N:-100}

now_ns() { date +%s%N; }
ms()    { awk -v n="$1" 'BEGIN{printf "%.1f", n / 1e6}'; }
ratio() { awk -v a="$1" -v b="$2" 'BEGIN{if (b <= 0) print "N/A"; else printf "%.2fx", a / b}'; }

perf_side() { # $1=dir → 输出 "create_ns update_ns delete_ns"
  local dir=$1 c u del t0 t1 i
  rm -rf "$dir"
  mkdir -p "$dir"

  t0=$(now_ns)
  for ((i = 0; i < PERF_N; i++)); do
    printf 'create-%08d\n' "$i" >"$dir/f$i" || return 1
  done
  t1=$(now_ns); c=$((t1 - t0))

  t0=$(now_ns)
  for ((i = 0; i < PERF_N; i++)); do
    printf 'update-%08d\n' "$i" >"$dir/f$i" || return 1
  done
  t1=$(now_ns); u=$((t1 - t0))

  t0=$(now_ns)
  rm -f "$dir"/f* || return 1
  t1=$(now_ns); del=$((t1 - t0))

  echo "$c $u $del"
}

test_perf() {
  sec "测试 3：性能对比（新增/更新/删除 × $PERF_N 个文件，FUSE vs 无 FUSE 基线）"
  local d=$TMP/t3
  mkdir -p "$d/mnt" "$d/upper" "$d/work" "$d/base"

  start_daemon "$d/mnt" "$d/upper" "$d/work" "$d/daemon.log"
  if ! wait_mounted "$d/mnt"; then
    bad "挂载失败，跳过性能测试，daemon 日志："
    dump_log "$d/daemon.log"
    DPID=""
    return
  fi

  local o b oc ou od bc bu bd
  o=$(perf_side "$d/mnt/perf") ||
    { bad "FUSE 侧写入失败"; graceful_stop "$DPID"; DPID=""; return; }
  b=$(perf_side "$d/base/perf") ||
    { bad "基线侧写入失败"; graceful_stop "$DPID"; DPID=""; return; }
  graceful_stop "$DPID"; DPID=""

  read -r oc ou od <<<"$o"
  read -r bc bu bd <<<"$b"

  echo
  printf '%-10s %14s %14s %10s\n' "操作" "FUSE(ms)" "基线(ms)" "开销"
  printf '%-10s %14s %14s %10s\n' "新增" "$(ms "$oc")" "$(ms "$bc")" "$(ratio "$oc" "$bc")"
  printf '%-10s %14s %14s %10s\n' "更新" "$(ms "$ou")" "$(ms "$bu")" "$(ratio "$ou" "$bu")"
  printf '%-10s %14s %14s %10s\n' "删除" "$(ms "$od")" "$(ms "$bd")" "$(ratio "$od" "$bd")"
  ok "性能对比完成（开销为 FUSE 用户态往返引入，比例异常时可查 daemon.log）"
}

preflight
test_perf
summary

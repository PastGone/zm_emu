#!/usr/bin/env bash
# ============================================================================
# zm_emu 批量稳定性测试
#
#   逐个 applet 跑固定时长，统计：稳定 / 崩溃 / 早退，并打印崩溃点(err/PC)。
#
# 用法:
#   ./test_all.sh                      # 跑 applet/ 下所有带 .app 的目录（每个 6s）
#   ZM_TEST_SEC=30 ./test_all.sh       # 改单次时长
#   ZM_TEST_SEC=0  ./test_all.sh       # 不限时长（跑到 applet 自己退出）
#   ./test_all.sh 000004fe 00000506    # 只测指定目录名
#   ZM_TEST_DISPLAY=1 ./test_all.sh    # 用真实窗口（默认无头 SDL dummy，不弹窗）
#
# 环境变量:
#   ZM_TEST_SEC      每个 applet 运行秒数（默认 6；填 0 / inf / none 表示不限时长）
#   ZM_TEST_OUT      日志输出目录（默认 /tmp/zm_test）
#   ZM_TEST_DISPLAY  置 1 则用真实窗口
#
# 判定:
#   稳定 = 跑满时长且日志无“异常停止”
#   崩溃 = 日志出现“异常停止”
#   早退 = 未跑满、也未崩溃（自身提前退出）
#   退出 = 不限时长模式下 applet 自己退出且未崩溃（exit 码见备注）
#
# 注：目录名与 .app 名可能不同（如 000004051/00000405.app），
#     故对 zm_emu 传 .app 的**全路径**而非短名。
# ============================================================================
set -u
cd "$(dirname "$0")"

BIN="$(pwd)/build/linux/x86_64/release/zm_emu"
ROOT="$(pwd)/applet"
SEC="${ZM_TEST_SEC:-6}"
OUT="${ZM_TEST_OUT:-/tmp/zm_test}"
mkdir -p "$OUT"

if [ ! -x "$BIN" ]; then
  echo "找不到 $BIN，先执行: xmake build" >&2
  exit 1
fi

# 无头运行，避免批量弹窗；需要肉眼看画面时用 ZM_TEST_DISPLAY=1
if [ "${ZM_TEST_DISPLAY:-0}" = "1" ]; then
  DRV=()
else
  DRV=(SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy)
fi

# 时长控制：ZM_TEST_SEC=0/inf/none/off/unlimited → 不限时长（不加 timeout，
# 一直跑到 applet 自己退出）。注意：带事件循环的 applet 此时会一直跑，
# 想结束请 Ctrl-C，或配 ZM_TEST_DISPLAY=1 直接关窗口。
UNLIMITED=0
case "${SEC,,}" in
  0 | inf | none | off | unlimited)
    TL=()
    UNLIMITED=1
    ;;
  *)
    TL=(timeout -k 2 "$SEC")
    ;;
esac

# 收集待测 applet 目录名
list_apps() {
  if [ "$#" -gt 0 ]; then
    printf '%s\n' "$@"
    return
  fi
  for d in "$ROOT"/*/; do
    n="$(basename "$d")"
    if ls "$d"*.app >/dev/null 2>&1; then
      printf '%s\n' "$n"
    fi
  done
}

printf '%-12s %-8s %-5s %-18s %s\n' "applet" "结果" "秒" "err/PC" "备注"
printf -- '------------------------------------------------------------------------------------\n'

n_ok=0
n_crash=0
n_early=0
n_exit=0
for name in $(list_apps "$@"); do
  app="$(ls "$ROOT/$name"/*.app 2>/dev/null | head -1)"
  [ -n "$app" ] || continue
  log="$OUT/$name.log"

  printf '  ... 正在跑 %-12s ' "$name"
  t0=$(date +%s)
  # 限时模式用 timeout -k：SDL2 会接管 SIGTERM（转成 SDL_QUIT），applet 不
  # pump 事件时 SIGTERM 杀不掉，故到点 2 秒后强 SIGKILL，保证脚本不卡住。
  # 不限时长模式（TL 为空）则直接跑，等 applet 自己退出。
  # shellcheck disable=SC2086
  env ZM_APPLET="$app" ZM_LOG=error ${DRV[@]+"${DRV[@]}"} \
      ${TL[@]+"${TL[@]}"} "$BIN" >"$log" 2>&1
  rc=$?
  dur=$(( $(date +%s) - t0 ))
  printf '\r'

  err=""
  pc=""
  if grep -aq "异常停止" "$log"; then
    err="$(grep -a "异常停止" "$log" | tail -1 | sed 's/.*err=\([0-9]*\).*/\1/')"
    pc="$(grep -a "寄存器：" "$log" | tail -1 | sed 's/.*R15=0x\([0-9A-Fa-f]*\).*/\1/')"
  fi

  if [ -n "$err" ]; then
    printf '%-12s %-8s %-5s %-18s %s\n' "$name" "崩溃" "$dur" \
        "err=$err pc=0x$pc" "$log"
    n_crash=$((n_crash + 1))
  elif [ "$UNLIMITED" = "1" ]; then
    # 不限时长：没崩就是自己退出了（exit 码见备注）
    printf '%-12s %-8s %-5s %-18s %s\n' "$name" "退出" "$dur" "-" "exit=$rc"
    n_exit=$((n_exit + 1))
  elif [ "$rc" -eq 124 ]; then
    printf '%-12s %-8s %-5s %-18s %s\n' "$name" "稳定" "$dur" "-" "跑满 ${SEC}s"
    n_ok=$((n_ok + 1))
  else
    printf '%-12s %-8s %-5s %-18s %s\n' "$name" "早退" "$dur" "exit=$rc" "$log"
    n_early=$((n_early + 1))
  fi
done

echo
if [ "$UNLIMITED" = "1" ]; then
  echo "汇总(不限时长): 退出 $n_exit | 崩溃 $n_crash | 共 $((n_exit + n_crash))"
else
  echo "汇总: 稳定 $n_ok | 崩溃 $n_crash | 早退 $n_early | 共 $((n_ok + n_crash + n_early))"
fi
echo "日志: $OUT/<applet>.log"

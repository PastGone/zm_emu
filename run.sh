#!/usr/bin/env bash
# ============================================================================
# zm_emu 运行 / 诊断脚本
#
#   ./run.sh              等价于 ./run.sh play
#   ./run.sh help         看全部场景
#
# 可用环境变量：
#   ZM_APPLET=00000506    指定 applet（默认 00000506）
#   ZM_OUT=/tmp/zm        输出目录（截图 / 层图 / 日志）
# ============================================================================
set -u
cd "$(dirname "$0")"

BIN="build/linux/x86_64/release/zm_emu"
APPLET="${ZM_APPLET:-00000506}"
OUT="${ZM_OUT:-/tmp/zm}"
LOG="${ZM_OUT:-/tmp/zm}.log"

if [ ! -x "$BIN" ]; then
  echo "找不到可执行文件 $BIN，先编译：xmake build" >&2
  exit 1
fi

mkdir -p "$OUT"

# 统一入口：$1 = 场景名，其余走环境变量
# ZM_TIMEOUT=<秒> 可让程序到点自动退出（无人值守跑批时用）
run() {
  rm -f "$OUT"/*.png 2>/dev/null
  echo "── 场景: $1"
  echo "   applet : $APPLET"
  echo "   applet out: $OUT/  （日志 $LOG）"
  echo "   提示   : 关窗口即可退出；ZM_LOG 控制日志量"
  echo
  if [ -n "${ZM_TIMEOUT:-}" ]; then
    env ZM_APPLET="$APPLET" "${@:2}" timeout "$ZM_TIMEOUT" "$BIN" 2>&1 | tee "$LOG"
  else
    env ZM_APPLET="$APPLET" "${@:2}" "$BIN" 2>&1 | tee "$LOG"
  fi
  return 0
}

usage() {
  cat <<'EOF'
用法: ./run.sh [场景]

  play      【最常用】关掉日志，自己玩、看画面（最流畅）
  log       打开 info 日志，边玩边看（还能接受）
  debug     全量日志（含逐指令 HOOK，非常卡，只在抓疑难问题时用）
  slot      槽位调用统计：哪些虚表槽真被调用、哪些从未调用
  probe     层占比探针：每 60 帧报"层0/层1 里透明色 vs 实际内容"的占比
  dump      把每一层的缓冲存成 PNG（每 60 帧一张）到 $OUT/
  shot      跨帧截图存到 $OUT/（每 30 个 present 一张）
  help      显示这份说明

环境变量:
  ZM_APPLET=00000506   指定 applet
  ZM_OUT=/tmp/zm       输出目录

说明:
  play 默认 ZM_LOG=off —— 画面验证时务必用它，
  否则全量日志会明显拖慢（实测 8 秒能刷 34 万行、log.txt 写 23MB）。

注: 原先的 ab/old/orderold/noskip 场景依赖 ZM_NO_BASELAYER / ZM_LAYER_ORDER /
    ZM_NO_SKIP 这些"历史开关"，它们现已随 RE 落地而全部移除：
      - 层 0 按 ZMAEE_IDisplay_New 语义正式建立（zm_layer_init_base）
      - 合成顺序由 applet 的 UpdateEx layerList 决定，不再由宿主猜
      - 透明只有一条判据：层载荷 +0x2C 非 0 才 mask（见 fb_composite）
EOF
}

case "${1:-play}" in
  play)     run play      ZM_LOG=off ;;
  log)      run log       ZM_LOG=info ;;
  debug)    run debug     ZM_LOG=debug ;;
  slot)     run slot      ZM_LOG=info ;;
  probe)    run probe     ZM_LOG=info ZM_PROBE=1 ;;
  dump)     run dump      ZM_LOG=info ZM_PROBE=1 ZM_DUMP_LAYER="$OUT/L" ;;
  shot)     run shot      ZM_LOG=off ZM_SCREENSHOT="$OUT/S" ZM_SHOT_EVERY=30 ZM_SHOT_MAX=30 ;;

  help|-h|--help) usage ;;
  *) echo "未知场景: $1"; echo; usage; exit 1 ;;
esac

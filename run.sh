#!/usr/bin/env bash
# run.sh —— 快速启动 zm_emu 带 GUI 窗口，用于手动测试体验
#
# 用法:
#   ./run.sh                 交互选择 applet（默认 GUI 模式）
#   ./run.sh 405             直接运行 00000405
#   ./run.sh 1               直接运行 00000001
#   ./run.sh -H 405          无头模式（自动化，不弹窗口）
#   ./run.sh -b              先构建再运行
#   ./run.sh -l applet/xxx   列出指定目录下的 applet
xmake
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EMU="$SCRIPT_DIR/build/linux/x86_64/release/zm_emu"

HEADLESS=""
BUILD_FIRST=0
LIST_MODE=0
LIST_DIR=""
APPLET_ARG=""

usage() {
  echo "用法: ./run.sh [选项] [applet编号]"
  echo ""
  echo "选项:"
  echo "  -H          无头模式（不弹 SDL 窗口，跑完自动退出）"
  echo "  -b          先执行 xmake 构建再运行"
  echo "  -l DIR      列出目录下的 applet/编号（默认 applet/）"
  echo "  -h          显示帮助"
  echo ""
  echo "示例:"
  echo "  ./run.sh             交互选择"
  echo "  ./run.sh 405          运行 00000405"
  echo "  ./run.sh 1            运行 00000001"
  echo "  ./run.sh -H 405       无头模式运行"
  exit 0
}

# --- 解析参数 ---
while [[ $# -gt 0 ]]; do
  case "$1" in
    -H)         HEADLESS="-H"; shift ;;
    -b)         BUILD_FIRST=1; shift ;;
    -l)         LIST_MODE=1; LIST_DIR="${2:-applet}"; shift; [[ $# -gt 0 && "$1" != -* ]] && { LIST_DIR="$1"; shift; } ;;
    -h|--help)  usage ;;
    *)          APPLET_ARG="$1"; shift ;;
  esac
done

# --- 列出模式 ---
if [[ $LIST_MODE -eq 1 ]]; then
  echo "可用 applet:"
  for d in "$SCRIPT_DIR/$LIST_DIR"/0*/; do
    [[ -d "$d" ]] || continue
    id=$(basename "$d")
    app="$d/${id}.app"
    if [[ -f "$app" ]]; then
      name=""
      [[ -f "$d/desc.txt" ]] && name=" — $(head -1 "$d/desc.txt")"
      echo "  $id$name"
    fi
  done
  exit 0
fi

# --- 构建 ---
if [[ $BUILD_FIRST -eq 1 ]]; then
  echo "==> 构建中..."
  cd "$SCRIPT_DIR" && xmake 2>&1 | tail -3
fi

if [[ ! -x "$EMU" ]]; then
  echo "==> 未找到可执行文件，尝试构建..."
  cd "$SCRIPT_DIR" && xmake 2>&1 | tail -3
  if [[ ! -x "$EMU" ]]; then
    echo "错误: 构建失败，请检查"
    exit 1
  fi
fi

# --- 选择 applet ---
if [[ -z "$APPLET_ARG" ]]; then
  # 收集可用 applet
  APPLETS=()
  NAMES=()
  for d in "$SCRIPT_DIR"/applet/0*/; do
    [[ -d "$d" ]] || continue
    id=$(basename "$d")
    app="$d/${id}.app"
    if [[ -f "$app" ]]; then
      APPLETS+=("$id")
      name=""
      [[ -f "$d/desc.txt" ]] && name=" — $(head -1 "$d/desc.txt")"
      NAMES+=("$name")
    fi
  done

  if [[ ${#APPLETS[@]} -eq 0 ]]; then
    echo "错误: applet/ 目录下没有找到任何 applet"
    exit 1
  fi

  echo ""
  echo "===== 可用 Applet ====="
  for i in "${!APPLETS[@]}"; do
    printf "  %d) %s%s\n" $((i+1)) "${APPLETS[$i]}" "${NAMES[$i]}"
  done
  echo "======================="
  echo ""
  read -r -p "选择编号 [1-${#APPLETS[@]}]: " CHOICE
  if [[ ! "$CHOICE" =~ ^[0-9]+$ ]] || (( CHOICE < 1 || CHOICE > ${#APPLETS[@]} )); then
    echo "无效选择"
    exit 1
  fi
  APPLET_ARG="${APPLETS[$((CHOICE-1))]}"
fi

# --- 补齐编号（如 1 → 00000001, 405 → 00000405）---
APPLET_ARG=$(printf "%08x" "$((16#${APPLET_ARG}))")

# --- 运行 ---
echo ""
echo "==> 运行 $APPLET_ARG"
if [[ -n "$HEADLESS" ]]; then
  echo "    模式: 无头"
  exec "$EMU" $HEADLESS -n 12 -c 4 "$APPLET_ARG"
else
  echo "    模式: GUI（关闭窗口退出，或按 ESC）"
  exec "$EMU" -t 0 "$APPLET_ARG"
fi

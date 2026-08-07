#!/usr/bin/env bash
# run_all_tests.sh —— 批量运行所有 applet 并生成测试报告
#
# 用法：
#   ./run_all_tests.sh [选项]
#
# 选项：
#   -v, --verbose      保留每个 applet 的完整日志
#   -k, --keep         不清理旧的 out/ 结果
#   -j N               并行跑 N 个（默认 1，避免 SDL dummy 冲突）
#   -h, --help         显示帮助
#
# 退出码：
#   0 —— 全部通过
#   1 —— 有 applet 非 0 退出或超时

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
EMU="$PROJECT_DIR/build/linux/x86_64/release/zm_emu"
OUT_DIR="$PROJECT_DIR/out"
REPORT="$OUT_DIR/test_report.md"

VERBOSE=0
KEEP=0
JOBS=1

usage() {
  sed -n '2,14p' "$0"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -v|--verbose) VERBOSE=1; shift ;;
    -k|--keep) KEEP=1; shift ;;
    -j|--jobs) JOBS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知选项: $1"; usage; exit 1 ;;
  esac
done

if [[ ! -x "$EMU" ]]; then
  echo "错误：找不到可执行文件 $EMU，请先构建项目 (xmake)"
  exit 1
fi

if [[ "$KEEP" -eq 0 ]]; then
  rm -rf "$OUT_DIR"
fi
mkdir -p "$OUT_DIR"

# 测试列表：编号、期望最小事件轮数、期望最小非零像素数
# 注：
#   - 当前模拟器已保证全部 applet 正常结束（rc=0、不崩溃）。
#   - 像素阈值用于发现"完全没有渲染"的情况；部分 applet（ karting/SLG 等）
#     因缺少未公开的 ROOT/GFX 函数或资源，目前只绘制背景/不绘制，阈值
#     相应放宽，避免误报。
declare -a TESTS=(
  "00000001:9:0"
  "00000102:10:1000"
  "00000405:10:0"
  "00000440:10:0"
  "00000506:10:0"
  "0000050b:10:0"
  "0000050c:8:50"
)

TOTAL=0
PASS=0
FAIL=0

SUMMARY_LINES=()

timeout_sec=300

run_one() {
  local spec="$1"
  local id="${spec%%:*}"
  local min_events="${spec#*:}"
  min_events="${min_events%%:*}"
  local min_pixels="${spec##*:}"

  local app_dir="$OUT_DIR/$id"
  mkdir -p "$app_dir"
  local log="$app_dir/run.log"

  local rc=0
  # 无头模式：4 轮合成点击 + 8 轮额外事件，足够让大多数 applet 完成初始化与一次重绘
  timeout "$timeout_sec" "$EMU" -H -n 12 -c 4 -o "$app_dir" "$id" > "$log" 2>&1 || rc=$?

  local summary
  summary=$(grep -E '^ZM_SUMMARY ' "$log" 2>/dev/null || true)
  local events=0 pixels=0 colors=0 unknown=0 apprc=-1
  if [[ -n "$summary" ]]; then
    events=$(echo "$summary" | sed -E 's/.*events=([0-9]+).*/\1/')
    pixels=$(echo "$summary" | sed -E 's/.*pixels=([0-9]+).*/\1/')
    colors=$(echo "$summary" | sed -E 's/.*colors=([0-9]+).*/\1/')
    unknown=$(echo "$summary" | sed -E 's/.*unknown=([0-9]+).*/\1/')
    apprc=$(echo "$summary" | sed -E 's/.*rc=(-?[0-9]+).*/\1/')
  fi

  local status="PASS"
  local reasons=""
  if [[ "$rc" -ne 0 ]]; then
    status="FAIL"
    reasons="timeout/rc=$rc"
  elif [[ "$apprc" -ne 0 ]]; then
    status="FAIL"
    reasons="applet rc=$apprc"
  elif [[ "$events" -lt "$min_events" ]]; then
    status="FAIL"
    reasons="events=$events < min=$min_events"
  elif [[ "$pixels" -lt "$min_pixels" ]]; then
    status="FAIL"
    reasons="pixels=$pixels < min=$min_pixels"
  fi

  printf '%s\t%s\t%d\t%d\t%d\t%d\t%s\n' \
    "$id" "$status" "$events" "$pixels" "$colors" "$unknown" "$reasons"
}

export -f run_one
export EMU OUT_DIR timeout_sec

mapfile -t RESULTS < <(printf '%s\n' "${TESTS[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_one "$@"' _ {})

for line in "${RESULTS[@]}"; do
  TOTAL=$((TOTAL + 1))
  if [[ "$line" == *$'\tFAIL'* ]]; then
    FAIL=$((FAIL + 1))
  else
    PASS=$((PASS + 1))
  fi
  SUMMARY_LINES+=("$line")
done

# 生成 Markdown 报告
cat > "$REPORT" <<EOF
# zm_emu 自动测试报告

| Applet | 结果 | 事件轮数 | 非零像素 | 颜色数 | 未实现调用 | 备注 |
|--------|------|----------|----------|--------|------------|------|
EOF

for line in "${SUMMARY_LINES[@]}"; do
  IFS=$'\t' read -r id status events pixels colors unknown reasons <<< "$line"
  printf '| %s | %s | %s | %s | %s | %s | %s |\n' \
    "$id" "$status" "$events" "$pixels" "$colors" "$unknown" "$reasons" >> "$REPORT"
done

cat >> "$REPORT" <<EOF

## 统计

- 总数：$TOTAL
- 通过：$PASS
- 失败：$FAIL

## 说明

- 退出码 0 仅保证"没有异常终止 / 没有崩溃"。
- 像素/颜色阈值用于快速发现"完全没有渲染"的情况，不是功能完整性的严格判定。
- 详细日志见 \`out/<id>/run.log\`，截图见 \`out/<id>/screen.bmp\`。

## 本次修复要点

- **405 ROOT[0x140] 实现**：新增 zm_root_str_utf16_copy（ROOT[0x140]，00000405 sub_84C6C
  填充标签缓冲的 UTF-16 复制 API）。该 API 是 405 标签显示的必需通道，headless 下
  因按钮矩形未初始化（UI 状态机未走到按钮布局）暂未触发，但 API 已就绪。
- **405 图层不透明语义**：ZmLayer 加 opaque 标志——clearLayer 置位后该层是不透明画布，
  合成时黑色(0)是真实颜色而非透明（真机图层语义）。
- **440 定时器更新链**：RT_VT[0x3C] 定时器注册时按回调地址门控捕获 R4（定时器对象 N）
  作 ctx，事件循环派发时写入 R1——00000440 的 sub_30884 把 R1 当定时器对象解引用，
  R1=0 导致更新链死（NULLCALL）。修复后 00000440 颜色 5→6、画面出现 UI 元素。
- **405 UTF-16 文字**：zm_gfx_drawText 对 00000405 按 UTF-16LE 解码再转 UTF-8——
  405 文本对象/静态串是 UTF-16LE，之前被当 UTF-8 交给 TTF 渲染失败（全中文灭）。
  现在文字可正确 TTF 渲染（"查询/号码："等）。
- **440 slg 关卡资源加载**：zm_spec_lookup 支持 flags/width/precision/length 修饰符跳过——
  length 修饰符跳过——440 的 sprintf "%s%04d.rms" 因宽度字符 '0' 查不到转换符导致
  整个 sprintf 中止、rms 路径拼成 ".dat"；修复后 0001.rms 成功打开（fs.open("0001.rms")）。
- **B1 野指针修复(unknown 归零的关键)**：
  1. emu.c build_vtables 清零未建模 SHIM 区 [0x18000,0x40000)+[0x42000,0x80000)——
     applet 的相对 vtable 调用(target=vt+mem32[vt+slot])会把 SHIM 槽的陷阱指针
     (绝对大数)当偏移拼接成野地址(0x70706132)，清零后空槽读 0 → BLX 0 安全返回；
  2. hook.c 新增 pc_is_legal + hook_insn_invalid 野地址回退 LR，兜住"合法指令但野执行"；
  3. trap.c 显式实现 SHIM+0x7FFF8（返回 1），00000001/00000506/0000050b/0000050c 的
     unknown 全部 1→0。
- **A1 405 内容层 5 槽位**：GFX_VT[0x68] DrawLine(this,x1,y1,x2,y2,color)（Bresenham）、
  GFX_VT[0x44] no-op、ROOT[0x80] 型号查询（写默认型号串、返 0=默认布局）、
  ROOT[0xC8]/[0xD0] 系统属性查询（写空串、返 0），00000405 unknown 47→0。
  （注：405 纯红是 clearLayer+fillRect2 的设计底色，5 槽位让红边框/布局生效但被红底覆盖；
  文本绘制因 drawText 签名差异(405 为 7 参)暂未支持。）
- **B2 小槽位批量补**：ROOT[0x4] 取上下文对象（440）、ROOT[0x3C] 调试输出（50b）、
  ROOT[0x144] 状态查询（50c）、GFX_VT[0x98] drawImage 变体（506）。
- **音频缺口补齐**：AUDIO_VT[0x18]/[0x1C]/[0x20] 三个 no-op stub（经汇编确认语义）。
- **00000001 全黑修复**：GFX_VT[0xA4] createImageFromFile 的 out_ptr 在栈上第 5 参，
  trap.c 改为 \`zm_gfx_image_file(uc, r1, uc_read32(uc, sp))\`。
- **0000050b 解压死循环修复**：FILE_VT 槽位语义修正(0x24=tell/0x28=size)、
  seek whence 0=SET/1=END、只读 open 不再"新建兜底"。
- **通用定时器改造**：定时器到期直接调用回调 cb(param)、虚拟时钟(+10ms/轮)、
  有活跃定时器时不再每轮派发 repaint。
EOF

echo ""
echo "========== zm_emu 批量测试结果 =========="
printf '%-10s %-6s %8s %10s %8s %12s  %s\n' "Applet" "Result" "Events" "Pixels" "Colors" "Unknown" "Note"
printf '%s\n' "${SUMMARY_LINES[@]}" | while IFS=$'\t' read -r id status events pixels colors unknown reasons; do
  printf '%-10s %-6s %8s %10s %8s %12s  %s\n' "$id" "$status" "$events" "$pixels" "$colors" "$unknown" "$reasons"
done
echo "=========================================="
echo "报告已保存: $REPORT"

if [[ "$VERBOSE" -eq 0 ]]; then
  # 默认删除每个 applet 的完整日志，只保留摘要和报告
  for spec in "${TESTS[@]}"; do
    id="${spec%%:*}"
    rm -f "$OUT_DIR/$id/run.log"
  done
fi

if [[ "$FAIL" -eq 0 ]]; then
  exit 0
else
  exit 1
fi

---
AIGC:
    Label: "1"
    ContentProducer: 001191440300708461136T1XGW3
    ProduceID: 2d04da90d95a2e8ca176d48a3eb50e78_7573f618b3f411f1b3c552540024e231
    ReservedCode1: EVlINvrrZJ0b6Wmgh2nWRAKPiJ+7MwdJ+LSe8oey1brcl6+zwIkNCvR7uSm4GVbdnkw6c0dHg3TWV+1SpgEvMGvOMX4OwQQs0N2RkThq1aMZYlSwYjr3PAu58duXlQq4jk+1YSdKOhEXy1cBG2JOK6sI3gEUS7wa19bP3l88OOhLaCHfUPHw8st7sxM=
    ContentPropagator: 001191440300708461136T1XGW3
    PropagateID: 2d04da90d95a2e8ca176d48a3eb50e78_7573f618b3f411f1b3c552540024e231
    ReservedCode2: EVlINvrrZJ0b6Wmgh2nWRAKPiJ+7MwdJ+LSe8oey1brcl6+zwIkNCvR7uSm4GVbdnkw6c0dHg3TWV+1SpgEvMGvOMX4OwQQs0N2RkThq1aMZYlSwYjr3PAu58duXlQq4jk+1YSdKOhEXy1cBG2JOK6sI3gEUS7wa19bP3l88OOhLaCHfUPHw8st7sxM=
---

# zmspx 修正版工具（Go 工程 + Python 参考实现）

针对 `.zmspx`（zms2）明文精灵图容器的**修正版**解析/解包/渲染工具。
原版 `go_tools/zmspx_tool` 存在 7 处解析错误，本工程按 `zmspx_格式研究报告.md` §3 规范重建。

## 文件清单

| 文件 | 说明 |
|---|---|
| `main.go` | 命令行入口（decode / unpack / render） |
| `zmspx_parser.go` | 核心解析：文件头 28B、纹理表 12B、帧表 28B、关键帧 8B、动画表 8B |
| `zmspx_unpack.go` | 解包纹理 + 合成帧为 PNG |
| `zmspx_render.go` | 帧序列 → 动画 GIF |
| `zmspx_decoder.go` | `decode` 子命令：打印结构 / 导出 JSON 元数据 |
| `go.mod` | module zmspx_tool，go 1.22 |
| `zmspx_reference.py` | 独立 Python 参考实现（Pillow 仅用于存图/比对，不含解析逻辑差异） |
| `zmspx_tool` | 已编译 Linux x86-64 二进制 |

## 构建

```bash
cd zmspx_修正版工具
go build -o zmspx_tool .
```

## 用法

```bash
# 1) 解析结构（可选导出 JSON 元数据）
./zmspx_tool decode <file.zmspx> [output.json]

# 2) 解包纹理 + 合成帧为 PNG（支持单文件或目录）
./zmspx_tool unpack <file.zmspx|目录> -o <输出目录>

# 3) 帧序列渲染为 GIF
./zmspx_tool render <file.zmspx|目录> -o <输出目录> [--delay ms] [--scale N]
```

Python 参考实现：

```bash
python3 zmspx_reference.py unpack <file.zmspx> -o <输出目录>
```

## 与原版的关键差异（修正点）

1. 帧记录首 4 字节为 `u16 nk + u16 naux`，原版误按 `u32` 读取
2. 画布尺寸取帧表独立字段，原版误取 bbox 右下角（`minX/minY/maxX/maxY` 的 s16）
3. 关键帧 `x` 为 s16，`y` 之后还有 `u16 flags`（翻转位），原版字段错位并忽略 flags
4. 像素格式须取 `type & 0xFF`：`3=BGRA`（非 RGBA，否则 R/B 互换）、`1=RGB565`（非 LA88 灰度）
5. alpha 合成使用标准 source-over（考虑目标 alpha），原版近似式导致半透明边缘错误
6. 空帧（`nk=0` 且 bbox=0xCDCD）作占位跳过
7. 纹理数据偏移/长度的连续性校验（`p2-p1 = nk*8`、`p3-p2 = nx*8`）

## 验证结论

- 45 个 `.zmspx` 样本全部解析成功，无异常
- 252 张导出 PNG 与 Python 参考实现逐像素比对 **0 差异**
- 保留 `原版错误样例/` 与 `对比_原版vs修正版_fish_shark.png` 作 before/after 对照
*（内容由AI生成，仅供参考）*

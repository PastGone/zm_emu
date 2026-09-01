# Go 工具集

本目录将项目相关工具按功能拆分成多个**独立的 Go 项目**，每个项目都有自己的 `go.mod`，
可以单独 `go build` / `go run`。

## 项目结构

| 目录 | 工具 | 功能 |
|------|------|------|
| `zmspx_tool/` | zmspx | 解析/解包/渲染 `.zmspx` (zms2) 精灵文件 |
| `zbmp_tool/`  | zbmp  | 解码 `.zbmp` 图片为 PNG |
| `zmr_tool/`   | zmr   | 解析 `.zmr` (zmr0) 容器，导出各数据块 |
| `applet_header_tool/` | applet_header | 解析 `.app` 文件头 |
| `python_zmspx/` | -    | Python 原版 zmspx 工具（参考/对照用） |

每个工具目录均自含 `go.mod`，构建命令：

```bash
cd zmspx_tool && go build -o zmspx .
cd zbmp_tool && go build -o zbmp .
cd zmr_tool && go build -o zmr .
cd applet_header_tool && go build -o applet_header .
```

## 各工具用法

### zmspx_tool（解析/解包/渲染 `.zmspx`）

```bash
# 解析并打印结构，可选导出 JSON 元数据
./zmspx decode <file.zmspx> [output.json]

# 解包: 提取纹理 PNG 和 metadata.json
./zmspx unpack <file.zmspx|目录> -o <输出目录>

# 渲染: 将各动画渲染为 GIF
./zmspx render <file.zmspx|目录> -o <输出目录> [--delay ms] [--scale N]

# 调试: 导出未量化的渲染帧 PNG (用于与 Python 对比)
./zmspx render <file.zmspx> -o <gif目录> --debug-frames <帧目录>
```

### zbmp_tool（解码 `.zbmp`）

```bash
./zbmp <file.zbmp> [out.png]   # 未指定输出时默认同名 .png
```

### zmr_tool（解析 `.zmr` zmr0 容器）

```bash
./zmr <file.zmr> [-o 输出目录]  # 打印块信息；-o 导出解压后的数据块
```

### applet_header_tool（解析 `.app` 头）

```bash
./applet_header <file.app> [更多.app...]  # 打印各文件头字段
```

## 与 Python 版本的产物一致性（zmspx）

已逐字节 / 逐像素对比验证：

- **metadata.json**: 与 Python `json.dump(..., indent=2, ensure_ascii=False)` 逐字节一致
  （含空数组序列化为 `[]` 而非 `null`，帧序列为数值数组而非 base64）。
- **纹理 PNG**: 解码像素与 Pillow 完全一致（覆盖 RGBA/LA 格式）。
- **渲染帧**: 归一化、alpha 合成、最近邻放大的帧像素与 Pillow 完全一致。

### 说明

- **GIF 字节级无法与 Pillow 完全一致**：GIF 调色板量化、LZW 压缩、帧去重均为编码器内部实现。
  Go 版按帧序列逐帧输出，渲染内容（像素）与 Pillow 一致，动画播放效果相同。
- **格式变体**：部分 `.zmspx`（如 `00000506/res/` 图标类）采用 `num_keyframes` 为 16 位的变体，
  Python 会 `struct.error` 崩溃；Go 版有越界保护不会崩溃（可正常提取纹理），该类帧语义未完全解码。

package main

// zmspx_unpack.go - 解包 .zmspx 文件: 提取纹理 PNG、帧 PNG 和元数据 JSON —— 修正版
//
// 修正点:
//   1. "RGBA" 模式: 32bpp 纹理在文件中是 B,G,R,A 字节序, 原版直接 copy 导致 R/B 通道互换,
//      现在按 BGRA 读取并交换为 NRGBA。
//   2. "LA" 模式: type=0x101 (257) 实际是 16bpp RGB565 小端, 原版当 LA88 灰度图导出,
//      现在按 RGB565 展开为 RGB。
//   3. 新增 frames/ 目录: 按关键帧(含取向 flags)把纹理合成为每帧 PNG。

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// extractTextureImage 从 zmspx 文件中提取单个纹理为 NRGBA 图像
// 使用 NRGBA (非预乘 alpha) 以保持原始像素字节与 Pillow 一致
func extractTextureImage(z *ZmspxFile, texIndex int) *image.NRGBA {
	tex := z.Textures[texIndex]
	w, h := tex.Width, tex.Height

	if w == 0 || h == 0 {
		img := image.NewNRGBA(image.Rect(0, 0, 1, 1))
		img.Set(0, 0, color.NRGBA{0, 0, 0, 0})
		return img
	}

	pixelData := z.GetPixelData(texIndex)
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	need := w * h * tex.BPP
	if len(pixelData) < need {
		fmt.Fprintf(os.Stderr, "    警告: 纹理 %d 数据不足 (需 %d, 实际 %d), 已跳过\n",
			texIndex, need, len(pixelData))
		return img
	}

	switch tex.PixelMode() {
	case "BGRA":
		// 32bpp: 字节序 B,G,R,A -> R,G,B,A
		for i := 0; i < w*h; i++ {
			img.Pix[i*4+0] = pixelData[i*4+2] // R
			img.Pix[i*4+1] = pixelData[i*4+1] // G
			img.Pix[i*4+2] = pixelData[i*4+0] // B
			img.Pix[i*4+3] = pixelData[i*4+3] // A
		}
	case "RGB565":
		// 16bpp 小端: 0xRRRRRGGGGGGBBBBB
		for i := 0; i < w*h; i++ {
			v := int(pixelData[i*2]) | int(pixelData[i*2+1])<<8
			r := (v >> 11) & 0x1F
			g := (v >> 5) & 0x3F
			b := v & 0x1F
			img.Pix[i*4+0] = uint8(r * 255 / 31)
			img.Pix[i*4+1] = uint8(g * 255 / 63)
			img.Pix[i*4+2] = uint8(b * 255 / 31)
			img.Pix[i*4+3] = 255
		}
	case "PAL8":
		// 8bpp 调色板: 本样例未出现, 暂无调色板来源, 先按灰度输出并提示
		fmt.Fprintf(os.Stderr, "    提示: 纹理 %d 为 8bpp 调色板格式, 未实现调色板查找\n", texIndex)
		for i := 0; i < w*h; i++ {
			img.Pix[i*4+0] = pixelData[i]
			img.Pix[i*4+1] = pixelData[i]
			img.Pix[i*4+2] = pixelData[i]
			img.Pix[i*4+3] = 255
		}
	default:
		copy(img.Pix, pixelData[:need])
	}
	return img
}

// unpack 解包单个 .zmspx 文件 (返回解析对象)
func unpack(path0, outputDir string) *ZmspxFile {
	data, err := os.ReadFile(path0)
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取失败: %v\n", err)
		return nil
	}
	z, err := parseZmspx(path0, data)
	if err != nil {
		fmt.Fprintf(os.Stderr, "解析失败: %v\n", err)
		return nil
	}
	name := baseName(path0)

	// 创建输出目录
	texDir := filepath.Join(outputDir, "textures")
	_ = os.MkdirAll(texDir, 0755)

	fmt.Printf("\n%s\n", strings.Repeat("=", 60))
	fmt.Printf("解包: %s.zmspx\n", name)
	fmt.Printf("  纹理: %d, 帧: %d, 动画: %d\n", z.NumTextures, z.NumFrames, z.NumAnimations)
	fmt.Printf("%s\n", strings.Repeat("=", 60))

	// ── 提取纹理 ──
	fmt.Printf("  提取 %d 个纹理...\n", z.NumTextures)
	for _, tex := range z.Textures {
		if tex.Width == 0 || tex.Height == 0 {
			fmt.Printf("    跳过纹理 %d: 零尺寸 (%dx%d)\n", tex.Index, tex.Width, tex.Height)
			continue
		}
		fname := fmt.Sprintf("tex_%04d_%dx%d_t%d", tex.Index, tex.Width, tex.Height, tex.Type)
		img := extractTextureImage(z, tex.Index)
		path := filepath.Join(texDir, fname+".png")
		f, err := os.Create(path)
		if err != nil {
			fmt.Printf("    错误: 纹理 %d 保存失败: %v\n", tex.Index, err)
			continue
		}
		if err := png.Encode(f, img); err != nil {
			f.Close()
			fmt.Printf("    错误: 纹理 %d 保存失败: %v\n", tex.Index, err)
			continue
		}
		f.Close()
	}

	// ── 合成并导出每一帧 (含关键帧取向) ──
	if z.NumFrames > 0 {
		frameDir := filepath.Join(outputDir, "frames")
		_ = os.MkdirAll(frameDir, 0755)
		textures := loadAllTextures(z)
		fmt.Printf("  合成 %d 帧...\n", z.NumFrames)
		for _, fr := range z.Frames {
			img := renderFrame(z, textures, fr.Index)
			if fr.CanvasWidth <= 0 || fr.CanvasHeight <= 0 {
				fmt.Printf("    提示: 帧 %d 为空帧占位 (bbox 未初始化, nk=%d)\n", fr.Index, fr.NumKeyframes)
			}
			p := filepath.Join(frameDir, fmt.Sprintf("frame_%02d_%dx%d.png",
				fr.Index, img.Rect.Dx(), img.Rect.Dy()))
			if err := saveFramePNG(p, img); err != nil {
				fmt.Printf("    错误: 帧 %d 保存失败: %v\n", fr.Index, err)
			}
		}
	}

	// ── 保存元数据 JSON ──
	jsonPath := filepath.Join(outputDir, "metadata.json")
	writeMetadataJSON(jsonPath, z)

	fmt.Printf("  输出目录: %s\n", outputDir)
	fmt.Printf("  元数据: %s\n", jsonPath)
	return z
}

// runZmspxUnpack 对应 zmspx_unpack.py main
func runZmspxUnpack(args []string) int {
	input := ""
	output := "./zmspx_unpacked"
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-o", "--output":
			if i+1 < len(args) {
				output = args[i+1]
				i++
			}
		default:
			if input == "" {
				input = args[i]
			}
		}
	}
	if input == "" {
		fmt.Println("用法: zmspx unpack <file.zmspx 或目录> -o <输出目录>")
		return 1
	}

	fi, err := os.Stat(input)
	if err != nil {
		fmt.Fprintf(os.Stderr, "错误: %v\n", err)
		return 1
	}

	if fi.IsDir() {
		entries, err := os.ReadDir(input)
		if err != nil {
			fmt.Fprintf(os.Stderr, "错误: %v\n", err)
			return 1
		}
		files := []string{}
		for _, e := range entries {
			if !e.IsDir() && strings.HasSuffix(e.Name(), ".zmspx") {
				files = append(files, e.Name())
			}
		}
		sort.Strings(files)
		if len(files) == 0 {
			fmt.Printf("错误: 目录 %s 中没有 .zmspx 文件\n", input)
			return 1
		}
		for _, fname := range files {
			filepath0 := filepath.Join(input, fname)
			name := baseName(fname)
			outputDir := filepath.Join(output, name)
			_ = os.MkdirAll(outputDir, 0755)
			unpack(filepath0, outputDir)
		}
		fmt.Printf("\n%s\n", strings.Repeat("=", 60))
		fmt.Printf("全部完成! 共解包 %d 个文件到: %s\n", len(files), output)
	} else {
		name := baseName(input)
		outputDir := filepath.Join(output, name)
		_ = os.MkdirAll(outputDir, 0755)
		unpack(input, outputDir)
	}
	return 0
}

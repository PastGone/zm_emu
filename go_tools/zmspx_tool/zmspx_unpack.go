package main

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

// zmspx_unpack.go - 解包 .zmspx 文件，提取纹理 PNG 和元数据 JSON
// 逻辑与 python_zmspx/zmspx_unpack.py 完全一致。

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
	mode := tex.PixelMode()
	img := image.NewNRGBA(image.Rect(0, 0, w, h))

	switch mode {
	case "RGBA":
		copy(img.Pix, pixelData[:w*h*4])
	case "LA":
		for i := 0; i < w*h; i++ {
			l := pixelData[i*2]
			a := pixelData[i*2+1]
			img.SetNRGBA(i%w, i/w, color.NRGBA{l, l, l, a})
		}
	case "L":
		for i := 0; i < w*h; i++ {
			l := pixelData[i]
			img.SetNRGBA(i%w, i/w, color.NRGBA{l, l, l, 255})
		}
	default:
		copy(img.Pix, pixelData[:w*h*4])
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

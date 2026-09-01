package main

import (
	"fmt"
	"image"
	"image/color"
	"image/gif"
	"image/png"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// zmspx_render.go - 将 .zmspx 动画渲染为 GIF
// 逻辑与 python_zmspx/zmspx_render.py 完全一致。

// pasteWithAlpha 将 src 以 alpha 合成到 dst 上 (与 Pillow paste 行为一致)
// 公式: out = (src*a + dst*(255-a) + 127) / 255, a = src 的 alpha
func pasteWithAlpha(dst, src *image.NRGBA, ox, oy int) {
	sb := src.Bounds()
	db := dst.Bounds()
	for y := sb.Min.Y; y < sb.Max.Y; y++ {
		dy := oy + y - sb.Min.Y
		if dy < db.Min.Y || dy >= db.Max.Y {
			continue
		}
		for x := sb.Min.X; x < sb.Max.X; x++ {
			dx := ox + x - sb.Min.X
			if dx < db.Min.X || dx >= db.Max.X {
				continue
			}
			sp := src.NRGBAAt(x, y)
			a := int(sp.A)
			if a == 0 {
				continue
			}
			if a == 255 {
				dst.SetNRGBA(dx, dy, sp)
				continue
			}
			dp := dst.NRGBAAt(dx, dy)
			out := color.NRGBA{
				R: uint8((int(sp.R)*a + int(dp.R)*(255-a) + 127) / 255),
				G: uint8((int(sp.G)*a + int(dp.G)*(255-a) + 127) / 255),
				B: uint8((int(sp.B)*a + int(dp.B)*(255-a) + 127) / 255),
				A: uint8((int(sp.A)*a + int(dp.A)*(255-a) + 127) / 255),
			}
			dst.SetNRGBA(dx, dy, out)
		}
	}
}

// renderFrame 渲染单帧画面: 在画布上按关键帧指示组合多个纹理
func renderFrame(z *ZmspxFile, textures map[int]*image.NRGBA, frameIndex int) *image.NRGBA {
	frame := z.Frames[frameIndex]
	canvas := image.NewNRGBA(image.Rect(0, 0, frame.CanvasWidth, frame.CanvasHeight))
	for _, kf := range frame.Keyframes {
		if tex, ok := textures[kf.TextureIndex]; ok {
			pasteWithAlpha(canvas, tex, kf.X, kf.Y)
		}
	}
	return canvas
}

// loadAllTextures 加载文件中所有纹理为 NRGBA 图像
func loadAllTextures(z *ZmspxFile) map[int]*image.NRGBA {
	textures := map[int]*image.NRGBA{}
	for _, tex := range z.Textures {
		if tex.Width == 0 || tex.Height == 0 {
			img := image.NewNRGBA(image.Rect(0, 0, 1, 1))
			img.Set(0, 0, color.NRGBA{0, 0, 0, 0})
			textures[tex.Index] = img
		} else {
			textures[tex.Index] = extractTextureImage(z, tex.Index)
		}
	}
	return textures
}

// scaleNearest 最近邻放大 (对应 Pillow Image.NEAREST)
func scaleNearest(img *image.NRGBA, scale int) *image.NRGBA {
	w, h := img.Rect.Dx(), img.Rect.Dy()
	nw, nh := w*scale, h*scale
	out := image.NewNRGBA(image.Rect(0, 0, nw, nh))
	for y := 0; y < nh; y++ {
		sy := y / scale
		for x := 0; x < nw; x++ {
			out.SetNRGBA(x, y, img.NRGBAAt(x/scale, sy))
		}
	}
	return out
}

// renderGif 将 .zmspx 文件的所有动画渲染为 GIF
func renderGif(filepath0, outputDir string, delay, scale int, debugDir string) []string {
	data, err := os.ReadFile(filepath0)
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取失败: %v\n", err)
		return nil
	}
	z, err := parseZmspx(filepath0, data)
	if err != nil {
		fmt.Fprintf(os.Stderr, "解析失败: %v\n", err)
		return nil
	}
	name := baseName(filepath0)
	_ = os.MkdirAll(outputDir, 0755)

	fmt.Printf("\n%s\n", strings.Repeat("=", 60))
	fmt.Printf("渲染: %s.zmspx\n", name)
	fmt.Printf("  纹理: %d, 帧: %d, 动画: %d\n", z.NumTextures, z.NumFrames, z.NumAnimations)
	fmt.Printf("  延迟: %dms, 放大: %dx\n", delay, scale)
	fmt.Printf("%s\n", strings.Repeat("=", 60))

	textures := loadAllTextures(z)

	gifPaths := []string{}

	for _, anim := range z.Animations {
		seq := anim.FrameSequence
		fmt.Printf("  Anim %d: 帧序列 = %v\n", anim.Index, seq)

		images := []*image.NRGBA{}
		maxW, maxH := 0, 0
		for _, frameIdx := range seq {
			if int(frameIdx) < z.NumFrames {
				img := renderFrame(z, textures, int(frameIdx))
				images = append(images, img)
				if img.Rect.Dx() > maxW {
					maxW = img.Rect.Dx()
				}
				if img.Rect.Dy() > maxH {
					maxH = img.Rect.Dy()
				}
			} else {
				fmt.Printf("    警告: 帧索引 %d 超出范围 (共 %d 帧)\n", frameIdx, z.NumFrames)
			}
		}

		if len(images) == 0 {
			fmt.Printf("    跳过: 无有效帧\n")
			continue
		}

		// 统一尺寸 (GIF 要求所有帧同尺寸)
		normalized := []*image.NRGBA{}
		for _, img := range images {
			if img.Rect.Dx() != maxW || img.Rect.Dy() != maxH {
				canvas := image.NewNRGBA(image.Rect(0, 0, maxW, maxH))
				x := (maxW - img.Rect.Dx()) / 2
				y := (maxH - img.Rect.Dy()) / 2
				pasteWithAlpha(canvas, img, x, y)
				normalized = append(normalized, canvas)
			} else {
				normalized = append(normalized, img)
			}
		}

		// 放大
		if scale > 1 {
			for i, img := range normalized {
				normalized[i] = scaleNearest(img, scale)
			}
		}

		// 调试: 导出未量化的渲染帧 (PNG)，用于与 Python 渲染帧对比
		if debugDir != "" {
			_ = os.MkdirAll(debugDir, 0755)
			for i, img := range normalized {
				p := filepath.Join(debugDir, fmt.Sprintf("%s_anim%d_frame%02d.png", name, anim.Index, i))
				if err := saveFramePNG(p, img); err != nil {
					fmt.Printf("    调试帧保存失败: %v\n", err)
				}
			}
		}

		// 保存 GIF
		gifPath := filepath.Join(outputDir, fmt.Sprintf("%s_anim%d.gif", name, anim.Index))
		if err := encodeGIF(gifPath, normalized, delay); err != nil {
			fmt.Printf("    保存失败: %v\n", err)
			continue
		}
		w, h := normalized[0].Rect.Dx(), normalized[0].Rect.Dy()
		fmt.Printf("    保存: %s (%dx%d, %d 帧)\n",
			filepath.Base(gifPath), w, h, len(normalized))
		gifPaths = append(gifPaths, gifPath)
	}

	fmt.Printf("\n  共生成 %d 个 GIF → %s\n", len(gifPaths), outputDir)
	return gifPaths
}

// encodeGIF 将 NRGBA 帧编码为 GIF (loop=0, disposal=2, duration=delay ms)
func encodeGIF(path string, frames []*image.NRGBA, delay int) error {
	// 构建调色板: 收集所有帧中出现的颜色
	pal := buildPalette(frames)

	// 转换为 Paletted
	paletted := make([]*image.Paletted, len(frames))
	for i, fr := range frames {
		p := image.NewPaletted(fr.Rect, pal)
		for y := fr.Rect.Min.Y; y < fr.Rect.Max.Y; y++ {
			for x := fr.Rect.Min.X; x < fr.Rect.Max.X; x++ {
				p.Set(x, y, fr.NRGBAAt(x, y))
			}
		}
		paletted[i] = p
	}

	// delay 毫秒 -> GIF 延迟单位 (1/100 秒)
	ticks := delay / 10
	if ticks < 1 {
		ticks = 1
	}
	disposal := make([]byte, len(frames))
	delays := make([]int, len(frames))
	for i := range frames {
		delays[i] = ticks
		disposal[i] = 2 // 每帧绘制前清除画布
	}

	g := &gif.GIF{
		Image:     paletted,
		Delay:     delays,
		LoopCount: 0,   // 0 = 无限循环
		Disposal:  disposal,
	}

	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return gif.EncodeAll(f, g)
}

// buildPalette 从所有帧构建颜色调色板 (全透明 -> 索引0透明)
func buildPalette(frames []*image.NRGBA) color.Palette {
	// 使用 map 收集颜色
	colorMap := map[uint32]int{}
	order := []uint32{}
	for _, fr := range frames {
		for y := fr.Rect.Min.Y; y < fr.Rect.Max.Y; y++ {
			for x := fr.Rect.Min.X; x < fr.Rect.Max.X; x++ {
				c := fr.NRGBAAt(x, y)
				key := uint32(c.R)<<24 | uint32(c.G)<<16 | uint32(c.B)<<8 | uint32(c.A)
				if _, ok := colorMap[key]; !ok {
					colorMap[key] = len(order)
					order = append(order, key)
				}
			}
		}
	}

	pal := color.Palette{}
	// 透明色放最前
	pal = append(pal, color.RGBA{0, 0, 0, 0})
	count := 1
	for _, key := range order {
		if count >= 256 {
			break
		}
		c := color.RGBA{
			R: uint8(key >> 24),
			G: uint8(key >> 16),
			B: uint8(key >> 8),
			A: uint8(key),
		}
		pal = append(pal, c)
		count++
	}
	// 若颜色超过 255 个，补足到接近 256 或截断
	return pal
}

// saveFramePNG 调试用: 将帧保存为 PNG (用于与 Python 渲染帧对比)
func saveFramePNG(path string, img *image.NRGBA) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

// renderAllInDirectory 批量渲染目录下所有 .zmspx 文件
func renderAllInDirectory(inputDir, outputDir string, delay, scale int, debugDir string) {
	entries, err := os.ReadDir(inputDir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "错误: %v\n", err)
		return
	}
	files := []string{}
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".zmspx") {
			files = append(files, e.Name())
		}
	}
	sort.Strings(files)
	if len(files) == 0 {
		fmt.Printf("错误: 目录 %s 中没有 .zmspx 文件\n", inputDir)
		return
	}
	for _, fname := range files {
		renderGif(filepath.Join(inputDir, fname), outputDir, delay, scale, debugDir)
	}
	fmt.Printf("\n%s\n", strings.Repeat("=", 60))
	fmt.Printf("全部完成! 共渲染 %d 个文件 → %s\n", len(files), outputDir)
}

// runZmspxRender 对应 zmspx_render.py main
func runZmspxRender(args []string) int {
	input := ""
	output := "./zmspx_gifs"
	delay := 80
	scale := 1
	debugDir := ""
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-o", "--output":
			if i+1 < len(args) {
				output = args[i+1]
				i++
			}
		case "--delay":
			if i+1 < len(args) {
				fmt.Sscanf(args[i+1], "%d", &delay)
				i++
			}
		case "--scale":
			if i+1 < len(args) {
				fmt.Sscanf(args[i+1], "%d", &scale)
				i++
			}
		case "--debug-frames":
			if i+1 < len(args) {
				debugDir = args[i+1]
				i++
			}
		default:
			if input == "" {
				input = args[i]
			}
		}
	}
	if input == "" {
		fmt.Println("用法: zmspx render <file.zmspx 或目录> -o <输出目录> [--delay ms] [--scale N] [--debug-frames <dir>]")
		return 1
	}

	fi, err := os.Stat(input)
	if err != nil {
		fmt.Fprintf(os.Stderr, "错误: %v\n", err)
		return 1
	}
	if fi.IsDir() {
		renderAllInDirectory(input, output, delay, scale, debugDir)
	} else {
		renderGif(input, output, delay, scale, debugDir)
	}
	return 0
}

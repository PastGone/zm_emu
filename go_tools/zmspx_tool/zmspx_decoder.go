package main

// zmspx_decoder.go - 便捷解码命令：解析 .zmspx 并打印结构 / 导出 JSON 元数据。
// 对应 Python 的 zmspx_parser.py 命令行入口。

import (
	"encoding/json"
	"fmt"
	"os"
)

// runZmspxDecode 解析单个 .zmspx 文件并打印信息，可选导出 JSON 元数据。
func runZmspxDecode(args []string) int {
	if len(args) < 1 {
		fmt.Println("用法: zmspx decode <file.zmspx> [output.json]")
		fmt.Println("  解析 .zmspx 文件并输出结构化 JSON 元数据")
		return 1
	}
	filepath := args[0]
	outputPath := ""
	if len(args) >= 2 {
		outputPath = args[1]
	}

	data, err := os.ReadFile(filepath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取失败: %v\n", err)
		return 1
	}
	z, err := parseZmspx(filepath, data)
	if err != nil {
		fmt.Fprintf(os.Stderr, "解析失败: %v\n", err)
		return 1
	}

	fmt.Println(z.String())

	fmt.Println("\n纹理:")
	for i, tex := range z.Textures {
		if i >= 10 {
			break
		}
		fmt.Printf("  [%3d] %5dx%-5d type=%d (%s) off=0x%06x size=%d\n",
			tex.Index, tex.Width, tex.Height, tex.Type, tex.PixelMode(),
			tex.DataOffset, tex.DataSize)
	}
	if z.NumTextures > 10 {
		fmt.Printf("  ... (%d more)\n", z.NumTextures-10)
	}

	fmt.Println("\n帧:")
	for i, frame := range z.Frames {
		if i >= 5 {
			break
		}
		texIDs := []int{}
		for _, kf := range frame.Keyframes {
			texIDs = append(texIDs, kf.TextureIndex)
		}
		fmt.Printf("  Frame %d: canvas=%dx%d, textures=%v\n",
			frame.Index, frame.CanvasWidth, frame.CanvasHeight, texIDs)
	}
	if z.NumFrames > 5 {
		fmt.Printf("  ... (%d more)\n", z.NumFrames-5)
	}

	fmt.Println("\n动画:")
	for _, anim := range z.Animations {
		fmt.Printf("  Anim %d: sequence=%v\n", anim.Index, anim.FrameSequence)
	}

	if outputPath != "" {
		b, err := json.MarshalIndent(z.ToDict(), "", "  ")
		if err != nil {
			fmt.Fprintf(os.Stderr, "JSON 编码失败: %v\n", err)
			return 1
		}
		if err := os.WriteFile(outputPath, b, 0644); err != nil {
			fmt.Fprintf(os.Stderr, "保存失败: %v\n", err)
			return 1
		}
		fmt.Printf("\n元数据已保存: %s\n", outputPath)
	}
	return 0
}

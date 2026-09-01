package main

import (
	"fmt"
	"os"
	"path/filepath"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Printf("用法: %s <输入文件> [输出PNG]\n", filepath.Base(os.Args[0]))
		os.Exit(1)
	}
	input := os.Args[1]
	output := ""
	if len(os.Args) >= 3 {
		output = os.Args[2]
	} else {
		ext := filepath.Ext(input)
		output = input[:len(input)-len(ext)] + ".png"
	}

	zbmp, err := DecodeZBMP(input)
	if err != nil {
		fmt.Fprintf(os.Stderr, "解码失败: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("解码成功: %dx%d, flags=0x%X, bpp=%d\n",
		zbmp.Header.Width, zbmp.Header.Height, zbmp.Header.Flags, zbmp.BPP)

	if err := zbmp.SavePNG(output); err != nil {
		fmt.Fprintf(os.Stderr, "保存 PNG 失败: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("已保存: %s\n", output)
}

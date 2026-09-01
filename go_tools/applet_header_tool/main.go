package main

import (
	"fmt"
	"os"
	"path/filepath"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Printf("用法: %s <file.app> [更多.app...]\n", filepath.Base(os.Args[0]))
		fmt.Println("  解析 .app 文件头并打印头部字段信息")
		os.Exit(1)
	}

	ok := 0
	for _, path := range os.Args[1:] {
		raw, err := os.ReadFile(path)
		if err != nil {
			fmt.Fprintf(os.Stderr, "读取失败 %s: %v\n", path, err)
			continue
		}
		h, err := parseAppHeader(raw)
		if err != nil {
			fmt.Fprintf(os.Stderr, "解析失败 %s: %v\n", path, err)
			continue
		}
		fmt.Printf("\n文件: %s\n", path)
		h.print()
		ok++
	}
	if ok == 0 {
		os.Exit(1)
	}
}

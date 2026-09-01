package main

import (
	"fmt"
	"os"
)

func usage() {
	fmt.Println("用法:")
	fmt.Println("  zmspx decode <file.zmspx> [output.json]")
	fmt.Println("  zmspx unpack <file.zmspx|目录> -o <输出目录>")
	fmt.Println("  zmspx render <file.zmspx|目录> -o <输出目录> [--delay ms] [--scale N]")
}

func main() {
	args := os.Args[1:]
	if len(args) == 0 {
		usage()
		os.Exit(1)
	}
	switch args[0] {
	case "decode":
		os.Exit(runZmspxDecode(args[1:]))
	case "unpack":
		os.Exit(runZmspxUnpack(args[1:]))
	case "render":
		os.Exit(runZmspxRender(args[1:]))
	default:
		usage()
		os.Exit(1)
	}
}

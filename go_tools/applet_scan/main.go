package main

// applet_scan - 扫描目录下的所有 .app 文件，调用头解析逻辑提取头部信息，
// 并将结果汇总输出为 CSV。
//
// 用法:
//   applet_scan <目录> [-o output.csv] [--no-recursive]
//
//   <目录>           要扫描的根目录（默认递归扫描其下所有子目录）
//   -o, --output     输出 CSV 路径，默认 ./applet_headers.csv
//   --no-recursive   仅扫描根目录一层，不进入子目录

import (
	"crypto/md5"
	"encoding/csv"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// csvColumns CSV 表头顺序（字段名与 src/tool/paser_info.h 的 AppletHeader 对齐）
var csvColumns = []string{
	"File",
	"Path",
	"MD5",
	"AppletID",
	"AppletID_Hex",
	"Version",
	"Version_Hex",
	"Flags_Hex",
	"PayloadSize",
	"AppName",
	"IconName",
	"ProgramUID_Hex",
	"ActivationType",
	"ActivationType_Hex",
	"ActivationKey_Hex",
	"UnknownDataLength",
	"MinScreenWidth",
	"MinScreenHeight",
	"MaxScreenWidth",
	"MaxScreenHeight",
	"Reserved0x010_Hex",
	"Unknown0x184_Hex",
}

// row 一条记录
type row struct {
	path string // 相对目录的路径，用于排序
	rec  []string
}

func main() {
	root := ""
	output := "applet_headers.csv"
	recursive := true

	args := os.Args[1:]
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-o", "--output":
			if i+1 < len(args) {
				output = args[i+1]
				i++
			}
		case "--no-recursive":
			recursive = false
		default:
			if root == "" {
				root = args[i]
			}
		}
	}

	if root == "" {
		fmt.Println("用法: applet_scan <目录> [-o output.csv] [--no-recursive]")
		os.Exit(1)
	}

	if fi, err := os.Stat(root); err != nil || !fi.IsDir() {
		fmt.Fprintf(os.Stderr, "错误: 不是有效目录: %s\n", root)
		os.Exit(1)
	}

	rows := []row{}
	parseErrs := 0

	walkFn := func(path string, d os.DirEntry, err error) error {
		if err != nil {
			fmt.Fprintf(os.Stderr, "访问失败 %s: %v\n", path, err)
			return nil
		}
		if d.IsDir() {
			if !recursive && path != root {
				return filepath.SkipDir
			}
			return nil
		}
		if !strings.EqualFold(filepath.Ext(path), ".app") {
			return nil
		}

		rec, perr := parseOne(root, path)
		if perr != nil {
			parseErrs++
			fmt.Fprintf(os.Stderr, "解析失败 %s: %v\n", path, perr)
			return nil
		}
		rel, _ := filepath.Rel(root, path)
		rows = append(rows, row{path: rel, rec: rec})
		return nil
	}

	if recursive {
		if err := filepath.WalkDir(root, walkFn); err != nil {
			fmt.Fprintf(os.Stderr, "扫描错误: %v\n", err)
			os.Exit(1)
		}
	} else {
		entries, _ := os.ReadDir(root)
		for _, e := range entries {
			_ = walkFn(filepath.Join(root, e.Name()), e, nil)
		}
	}

	sort.Slice(rows, func(i, j int) bool {
		return strings.ToLower(rows[i].path) < strings.ToLower(rows[j].path)
	})

	if err := writeCSV(output, rows); err != nil {
		fmt.Fprintf(os.Stderr, "写入 CSV 失败: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("扫描完成: %s\n", root)
	fmt.Printf("  成功: %d 个 .app 文件\n", len(rows))
	if parseErrs > 0 {
		fmt.Printf("  失败: %d 个\n", parseErrs)
	}
	fmt.Printf("  输出: %s (%d 列)\n", output, len(csvColumns))
}

// parseOne 读取并解析单个 .app 文件，返回 CSV 一行（不含表头）。
// root 用于计算相对路径填入 Path 列。
func parseOne(root, path string) ([]string, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	h, err := parseAppHeader(raw)
	if err != nil {
		return nil, err
	}
	rel, _ := filepath.Rel(root, path)
	sum := md5.Sum(raw)
	md5hex := hex.EncodeToString(sum[:])
	rec := []string{
		filepath.Base(path),
		rel,
		md5hex,
		fmt.Sprintf("%d", h.AppletID),
		fmt.Sprintf("0x%08X", h.AppletID),
		fmt.Sprintf("%d", h.Version),
		fmt.Sprintf("0x%08X", h.Version),
		fmt.Sprintf("0x%08X", h.Flags),
		fmt.Sprintf("%d", h.PayloadSize),
		h.AppName,
		h.IconName,
		hexBytes(h.ProgramUID),
		fmt.Sprintf("%d", h.ActivationType),
		fmt.Sprintf("0x%08X", h.ActivationType),
		hexBytes(h.ActivationKey),
		fmt.Sprintf("%d", h.UnknownDataLength),
		fmt.Sprintf("%d", h.MinScreenWidth),
		fmt.Sprintf("%d", h.MinScreenHeight),
		fmt.Sprintf("%d", h.MaxScreenWidth),
		fmt.Sprintf("%d", h.MaxScreenHeight),
		fmt.Sprintf("0x%08X", h.Reserved0x010),
		fmt.Sprintf("0x%08X", h.Unknown0x184),
	}
	return rec, nil
}

// writeCSV 写出 CSV（含表头）
func writeCSV(output string, rows []row) error {
	f, err := os.Create(output)
	if err != nil {
		return err
	}
	defer f.Close()

	w := csv.NewWriter(f)
	if err := w.Write(csvColumns); err != nil {
		return err
	}
	for _, r := range rows {
		if err := w.Write(r.rec); err != nil {
			return err
		}
	}
	w.Flush()
	return w.Error()
}

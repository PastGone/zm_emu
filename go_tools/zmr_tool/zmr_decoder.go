package main

// zmr_decoder.go - 解析 .zmr 文件（zmr0 容器格式）。
//
// 参考 docs/zmr格式.md：
//
//	Magic         0x00  4B   "zmr0"（唯一版本）
//	Block count   0x04  4B   u32 LE，数据块数量
//	Header size   0x08  4B   u32 LE = 12 + count×4，同时是数据区起始偏移
//	Offset table  0x0C  count×4B  u32 LE 数组，单调递增绝对偏移，每项标记一个块的结束位置
//	Data blocks   hdr_size  变长   每块由相邻偏移表项界定；首字节决定压缩方式
//	Footer        EOF−4  4B   u32 LE = 文件总大小（完整性校验）

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// zmrBlock 一个解压后的数据块
type zmrBlock struct {
	Index      int
	Offset     int    // 块在文件中的起始偏移
	Size       int    // 块原始大小（压缩后）
	Compressed bool   // 是否为 zlib 压缩
	Data       []byte // 解压后的数据
}

// zmrFile zmr0 容器解析结果
type zmrFile struct {
	Magic        string
	BlockCount   int
	HeaderSize   int
	Offsets      []int
	FooterSize   int
	Blocks       []zmrBlock
	Decompressed int // 解压后总字节数
	Compressed   int // 压缩后总字节数
}

// isZlib 判断块是否为 zlib 压缩（zlib 魔数以 0x78 开头）
func isZlib(data []byte) bool {
	return len(data) >= 2 && data[0] == 0x78 && (data[1]&0x20) == 0
}

// parseZmr 解析 zmr0 文件内容
func parseZmr(filename string, raw []byte) (*zmrFile, error) {
	z := &zmrFile{}

	if len(raw) < 12 {
		return nil, fmt.Errorf("文件过小")
	}
	z.Magic = string(raw[0:4])
	if z.Magic != "zmr0" {
		return nil, fmt.Errorf("无效的魔数: %q (期望 'zmr0')", z.Magic)
	}
	z.BlockCount = int(binary.LittleEndian.Uint32(raw[4:8]))
	z.HeaderSize = int(binary.LittleEndian.Uint32(raw[8:12]))

	// 校验 header size
	if z.HeaderSize != 12+z.BlockCount*4 {
		return nil, fmt.Errorf("Header size 异常: %d (期望 %d)", z.HeaderSize, 12+z.BlockCount*4)
	}
	if z.HeaderSize > len(raw) {
		return nil, fmt.Errorf("Header size 超出文件大小: %d > %d", z.HeaderSize, len(raw))
	}

	// 读取偏移表
	z.Offsets = make([]int, z.BlockCount)
	for i := 0; i < z.BlockCount; i++ {
		z.Offsets[i] = int(binary.LittleEndian.Uint32(raw[12+i*4:]))
	}

	// 文件尾 4 字节 = 文件总大小
	if len(raw) >= 4 {
		z.FooterSize = int(binary.LittleEndian.Uint32(raw[len(raw)-4:]))
	}

	// 解析每个数据块
	for i := 0; i < z.BlockCount; i++ {
		start := z.HeaderSize
		if i > 0 {
			start = z.Offsets[i-1]
		}
		end := z.Offsets[i]
		if end > len(raw) {
			end = len(raw)
		}
		if start < 0 || start > end {
			continue
		}
		blkData := raw[start:end]

		b := zmrBlock{
			Index:  i,
			Offset: start,
			Size:   len(blkData),
		}

		if isZlib(blkData) {
			// zlib 解压
			out, err := zlibDecompress(blkData)
			if err == nil {
				b.Compressed = true
				b.Data = out
			} else {
				// 解压失败则视为原始数据
				b.Data = blkData
			}
		} else {
			// 未压缩原始数据
			b.Data = blkData
		}

		z.Compressed += b.Size
		z.Decompressed += len(b.Data)
		z.Blocks = append(z.Blocks, b)
	}

	return z, nil
}

func zlibDecompress(data []byte) ([]byte, error) {
	r, err := zlib.NewReader(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	defer r.Close()
	return io.ReadAll(r)
}

// printInfo 打印容器结构信息
func (z *zmrFile) printInfo() {
	fmt.Println("========== zmr0 容器信息 ==========")
	fmt.Printf("Magic        : %s\n", z.Magic)
	fmt.Printf("Block count  : %d\n", z.BlockCount)
	fmt.Printf("Header size  : %d\n", z.HeaderSize)
	fmt.Printf("Footer size  : %d\n", z.FooterSize)
	fmt.Printf("压缩前总大小 : %d 字节\n", z.Compressed)
	fmt.Printf("解压后总大小 : %d 字节\n", z.Decompressed)
	fmt.Println("----------------------------------")
	for _, b := range z.Blocks {
		mode := "原始"
		if b.Compressed {
			mode = "zlib"
		}
		fmt.Printf("  block %3d: off=0x%06x 压缩=%6d -> 解压=%6d (%s)\n",
			b.Index, b.Offset, b.Size, len(b.Data), mode)
	}
	fmt.Println("==================================")
}

// extract 导出解压后的块
func (z *zmrFile) extract(outputDir string) error {
	if err := os.MkdirAll(outputDir, 0755); err != nil {
		return err
	}
	for _, b := range z.Blocks {
		path := filepath.Join(outputDir, fmt.Sprintf("block_%04d.bin", b.Index))
		if err := os.WriteFile(path, b.Data, 0644); err != nil {
			return err
		}
	}
	return nil
}

func main() {
	if len(os.Args) < 2 {
		fmt.Printf("用法: %s <file.zmr> [-o 输出目录]\n", filepath.Base(os.Args[0]))
		fmt.Println("  解析 zmr0 容器，打印各块信息；-o 指定目录导出解压后的数据块")
		os.Exit(1)
	}
	input := os.Args[1]
	outputDir := ""
	for i := 1; i < len(os.Args); i++ {
		if os.Args[i] == "-o" && i+1 < len(os.Args) {
			outputDir = os.Args[i+1]
		}
	}

	raw, err := os.ReadFile(input)
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取失败: %v\n", err)
		os.Exit(1)
	}
	z, err := parseZmr(input, raw)
	if err != nil {
		fmt.Fprintf(os.Stderr, "解析失败: %v\n", err)
		os.Exit(1)
	}

	z.printInfo()

	if outputDir != "" {
		if err := z.extract(outputDir); err != nil {
			fmt.Fprintf(os.Stderr, "导出失败: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("已导出 %d 个数据块到: %s\n", len(z.Blocks), outputDir)
	}
}

package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
)

/*  已验证
go run . /home/apollo/文档/古时游戏/zmaee_emu/applet/00000102/icon.zbmp ./out.png
解码成功: 24x24, flags=0x3, bpp=4
已保存: ./out.png
*/

// ZBMPHeader 文件头（16 字节，小端序）
type ZBMPHeader struct {
	Magic  [4]byte // "ZMBM"
	Width  uint16
	Height uint16
	Flags  uint32
}

// ZBMP 解码结果
type ZBMP struct {
	Header ZBMPHeader
	Image  *image.RGBA
	BPP    int // 每像素字节数（2 或 4）
}

// DecodeZBMP 解码 ZBMP 文件，逻辑与 Python 改进版完全一致
func DecodeZBMP(filename string) (*ZBMP, error) {
	data, err := os.ReadFile(filename)
	if err != nil {
		return nil, err
	}
	if len(data) < 16 {
		return nil, fmt.Errorf("文件过小")
	}

	var header ZBMPHeader
	if err := binary.Read(bytes.NewReader(data[:16]), binary.LittleEndian, &header); err != nil {
		return nil, err
	}
	if string(header.Magic[:]) != "ZMBM" {
		return nil, fmt.Errorf("无效的魔数")
	}

	width := int(header.Width)
	height := int(header.Height)
	flags := header.Flags
	pixelData := data[16:]
	expectedPixels := width * height

	// ---- bpp 判定逻辑（与 Python 完全一致） ----
	var bpp int
	if flags&0x0100 != 0 {
		bpp = 4
	} else if flags&0x0002 != 0 {
		bpp = 4
	} else if len(pixelData) >= expectedPixels*4 {
		bpp = 4
	} else {
		bpp = 2
	}
	// -------------------------------------------

	img := image.NewRGBA(image.Rect(0, 0, width, height))

	if bpp == 4 {
		// 32 位 BGRA
		maxPixels := expectedPixels
		if len(pixelData)/4 < maxPixels {
			maxPixels = len(pixelData) / 4 // 数据不足时取可用像素
		}
		for i := 0; i < maxPixels; i++ {
			offset := i * 4
			b := pixelData[offset]
			g := pixelData[offset+1]
			r := pixelData[offset+2]
			a := pixelData[offset+3]
			x := i % width
			y := i / width
			img.SetRGBA(x, y, color.RGBA{r, g, b, a})
		}
	} else {
		// 16 位 RGB565
		maxPixels := expectedPixels
		if len(pixelData)/2 < maxPixels {
			maxPixels = len(pixelData) / 2
		}
		for i := 0; i < maxPixels; i++ {
			offset := i * 2
			pixel := binary.LittleEndian.Uint16(pixelData[offset : offset+2])
			r8 := uint8((pixel>>11)&0x1F) << 3
			g8 := uint8((pixel>>5)&0x3F) << 2
			b8 := uint8(pixel&0x1F) << 3
			a8 := uint8(255)
			if pixel == 0 {
				a8 = 0
			}
			x := i % width
			y := i / width
			img.SetRGBA(x, y, color.RGBA{r8, g8, b8, a8})
		}
	}
	// 注意：如果像素数不足，未填充的部分保持默认透明黑色（Go 的 NewRGBA 初始为全零）
	// 这与 Python 的 putdata 行为一致（不足时图片剩余部分为黑色透明）

	return &ZBMP{
		Header: header,
		Image:  img,
		BPP:    bpp,
	}, nil
}

// SavePNG 保存为 PNG
func (z *ZBMP) SavePNG(outputPath string) error {
	if z.Image == nil {
		return fmt.Errorf("无图像数据")
	}
	f, err := os.Create(outputPath)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, z.Image)
}

func main() {
	if len(os.Args) < 2 {
		fmt.Printf("用法: %s <输入文件> [输出PNG]\n", os.Args[0])
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

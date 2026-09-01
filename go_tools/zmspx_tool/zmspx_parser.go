package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// ZMSPX (zms2) 文件格式解析库，逻辑与 python_zmspx/zmspx_parser.py 完全一致。

const (
	HeaderSize        = 28
	TextureRecordSize = 12
	FrameRecordSize   = 28
	AnimRecordSize    = 8
	KeyframeEntrySize = 8
)

// PixelFormat 像素类型映射: type -> 每像素字节数等
type PixelFormat struct {
	BPP  int
	Mode string
	Desc string
}

var pixelFormats = map[uint16]PixelFormat{
	3:   {4, "RGBA", "RGBA8888 (真彩色+透明度)"},
	257: {2, "LA", "LA88 (灰度+透明度)"},
	1:   {1, "L", "L8 (灰度)"},
}

// Texture 单个纹理的信息
type Texture struct {
	Index      int
	Width      int
	Height     int
	Type       int // 3=RGBA8888, 257=LA88, 1=L8
	BPP        int // 每像素字节数
	DataOffset int // 像素数据在文件中的偏移量
	DataSize   int // 像素数据大小
}

func (t *Texture) PixelMode() string {
	if f, ok := pixelFormats[uint16(t.Type)]; ok {
		return f.Mode
	}
	return "RGBA"
}

// ExpectedSize 根据宽高和bpp计算的预期数据大小
func (t *Texture) ExpectedSize() int {
	return t.Width * t.Height * t.BPP
}

// Keyframe 关键帧条目: 将某个纹理放置到画布的指定位置
type Keyframe struct {
	TextureIndex int // 使用的纹理索引
	X            int // 画布上的 X 坐标 (原字段名: param)
	Y            int // 画布上的 Y 坐标 (原字段名: value)
}

// Frame 一帧画面: 在画布上组合多个纹理
type Frame struct {
	Index         int
	NumKeyframes  int
	CanvasWidth   int
	CanvasHeight  int
	Keyframes     []Keyframe
}

// Animation 一段动画: 帧索引播放序列
type Animation struct {
	Index         int
	DataSize      int     // 动画数据大小 (字节)
	DataOffset    int     // 动画数据在文件中的偏移量
	FrameSequence []uint8 // uint8 帧索引序列
}

// ZmspxFile 解析并持有 .zmspx (zms2) 文件的全部数据
type ZmspxFile struct {
	Filepath      string
	RawData       []byte
	Magic         string
	NumTextures   int
	NumFrames     int
	NumAnimations int
	FileSize      int
	Textures      []Texture
	Frames        []Frame
	Animations    []Animation
}

// parseZmspx 解析 .zmspx 文件，逻辑与 Python _parse 一致
func parseZmspx(filepath string, data []byte) (*ZmspxFile, error) {
	z := &ZmspxFile{
		Filepath: filepath,
		RawData:  data,
		FileSize: len(data),
	}

	if len(data) < HeaderSize {
		return nil, fmt.Errorf("文件过小")
	}

	// ── 1. 解析文件头 ──
	z.Magic = string(data[0:4])
	if z.Magic != "zms2" {
		return nil, fmt.Errorf("无效的魔数: %s (期望 'zms2')", z.Magic)
	}
	z.NumTextures = int(binary.LittleEndian.Uint32(data[4:8]))
	z.NumFrames = int(binary.LittleEndian.Uint32(data[12:16]))
	z.NumAnimations = int(binary.LittleEndian.Uint32(data[20:24]))

	// ── 2. 解析纹理表 ──
	texTableOffset := HeaderSize
	for i := 0; i < z.NumTextures; i++ {
		off := texTableOffset + i*TextureRecordSize
		if off+TextureRecordSize > len(data) {
			break
		}
		w := int(binary.LittleEndian.Uint16(data[off:]))
		h := int(binary.LittleEndian.Uint16(data[off+2:]))
		typ := int(binary.LittleEndian.Uint16(data[off+4:]))
		dataOff := int(binary.LittleEndian.Uint32(data[off+8:]))

		bpp := 4
		if f, ok := pixelFormats[uint16(typ)]; ok {
			bpp = f.BPP
		}

		var dataSize int
		if i < z.NumTextures-1 {
			nextOff := texTableOffset + (i+1)*TextureRecordSize
			if nextOff+TextureRecordSize <= len(data) {
				nextDataOff := int(binary.LittleEndian.Uint32(data[nextOff+8:]))
				dataSize = nextDataOff - dataOff
			}
		} else {
			trailingStart := z.FileSize
			if z.NumFrames > 0 {
				frameTableOff := texTableOffset + z.NumTextures*TextureRecordSize
				if frameTableOff+16+4 <= len(data) {
					trailingStart = int(binary.LittleEndian.Uint32(data[frameTableOff+16:]))
				}
			}
			dataSize = trailingStart - dataOff
		}

		z.Textures = append(z.Textures, Texture{
			Index: i, Width: w, Height: h, Type: typ,
			BPP: bpp, DataOffset: dataOff, DataSize: dataSize,
		})
	}

	// ── 3. 解析帧表 ──
	frameTableOffset := texTableOffset + z.NumTextures*TextureRecordSize
	for i := 0; i < z.NumFrames; i++ {
		off := frameTableOffset + i*FrameRecordSize
		if off+FrameRecordSize > len(data) {
			break
		}
		numKf := int(binary.LittleEndian.Uint32(data[off:]))
		canvasW := int(binary.LittleEndian.Uint16(data[off+12:]))
		canvasH := int(binary.LittleEndian.Uint16(data[off+14:]))
		kfOff1 := int(binary.LittleEndian.Uint32(data[off+16:]))

		keyframes := []Keyframe{}
		for j := 0; j < numKf; j++ {
			kfOff := kfOff1 + j*KeyframeEntrySize
			if kfOff+KeyframeEntrySize > len(data) {
				break // 越界保护，防止崩溃 (Python 此处会 struct.error)
			}
			texIdx := int(binary.LittleEndian.Uint16(data[kfOff:]))
			x := int(binary.LittleEndian.Uint16(data[kfOff+2:]))
			y := int(binary.LittleEndian.Uint32(data[kfOff+4:]))
			keyframes = append(keyframes, Keyframe{
				TextureIndex: texIdx, X: x, Y: y,
			})
		}

		z.Frames = append(z.Frames, Frame{
			Index: i, NumKeyframes: numKf,
			CanvasWidth: canvasW, CanvasHeight: canvasH,
			Keyframes: keyframes,
		})
	}

	// ── 4. 解析动画表 ──
	animTableOffset := frameTableOffset + z.NumFrames*FrameRecordSize
	for i := 0; i < z.NumAnimations; i++ {
		off := animTableOffset + i*AnimRecordSize
		if off+AnimRecordSize > len(data) {
			break
		}
		animSize := int(binary.LittleEndian.Uint32(data[off:]))
		animDataOff := int(binary.LittleEndian.Uint32(data[off+4:]))

		frameSeq := []uint8{}
		if animDataOff >= 0 && animDataOff+animSize <= len(data) {
			frameSeq = append(frameSeq, data[animDataOff:animDataOff+animSize]...)
		}

		z.Animations = append(z.Animations, Animation{
			Index: i, DataSize: animSize,
			DataOffset: animDataOff, FrameSequence: frameSeq,
		})
	}

	return z, nil
}

// GetPixelData 获取指定纹理的原始像素数据
func (z *ZmspxFile) GetPixelData(textureIndex int) []byte {
	tex := z.Textures[textureIndex]
	end := tex.DataOffset + tex.ExpectedSize()
	if tex.DataOffset < 0 || end > len(z.RawData) {
		return nil
	}
	return z.RawData[tex.DataOffset:end]
}

// ── JSON 序列化 (与 Python to_dict 完全一致) ──

type textureDict struct {
	Index      int `json:"index"`
	Width      int `json:"width"`
	Height     int `json:"height"`
	Type       int `json:"type"`
	BPP        int `json:"bpp"`
	DataOffset int `json:"data_offset"`
	DataSize   int `json:"data_size"`
}

type keyframeDict struct {
	TextureIndex int `json:"texture_index"`
	X            int `json:"x"`
	Y            int `json:"y"`
}

type frameDict struct {
	Index         int            `json:"index"`
	NumKeyframes  int            `json:"num_keyframes"`
	CanvasWidth   int            `json:"canvas_width"`
	CanvasHeight  int            `json:"canvas_height"`
	Keyframes     []keyframeDict `json:"keyframes"`
}

type animDict struct {
	Index         int   `json:"index"`
	DataSize      int   `json:"data_size"`
	DataOffset    int   `json:"data_offset"`
	FrameSequence []int `json:"frame_sequence"`
}

type zmspxDict struct {
	Magic         string         `json:"magic"`
	NumTextures   int            `json:"num_textures"`
	NumFrames     int            `json:"num_frames"`
	NumAnimations int            `json:"num_animations"`
	FileSize      int            `json:"file_size"`
	Textures      []textureDict  `json:"textures"`
	Frames        []frameDict    `json:"frames"`
	Animations    []animDict     `json:"animations"`
}

// ToDict 转换为可序列化的结构 (用于 JSON 导出)
// 注: 所有切片初始化为空切片 (而非 nil)，确保空列表序列化为 [] 而非 null，与 Python 一致
func (z *ZmspxFile) ToDict() zmspxDict {
	d := zmspxDict{
		Magic:         z.Magic,
		NumTextures:   z.NumTextures,
		NumFrames:     z.NumFrames,
		NumAnimations: z.NumAnimations,
		FileSize:      z.FileSize,
		Textures:      []textureDict{},
		Frames:        []frameDict{},
		Animations:    []animDict{},
	}
	for _, t := range z.Textures {
		d.Textures = append(d.Textures, textureDict{
			Index: t.Index, Width: t.Width, Height: t.Height,
			Type: t.Type, BPP: t.BPP,
			DataOffset: t.DataOffset, DataSize: t.DataSize,
		})
	}
	for _, f := range z.Frames {
		fd := frameDict{
			Index: f.Index, NumKeyframes: f.NumKeyframes,
			CanvasWidth: f.CanvasWidth, CanvasHeight: f.CanvasHeight,
			Keyframes: []keyframeDict{},
		}
		for _, kf := range f.Keyframes {
			fd.Keyframes = append(fd.Keyframes, keyframeDict{
				TextureIndex: kf.TextureIndex, X: kf.X, Y: kf.Y,
			})
		}
		d.Frames = append(d.Frames, fd)
	}
	for _, a := range z.Animations {
		fs := make([]int, len(a.FrameSequence))
		for i, v := range a.FrameSequence {
			fs[i] = int(v)
		}
		d.Animations = append(d.Animations, animDict{
			Index: a.Index, DataSize: a.DataSize,
			DataOffset: a.DataOffset, FrameSequence: fs,
		})
	}
	return d
}

// String 返回类似 Python __repr__ 的摘要
func (z *ZmspxFile) String() string {
	return fmt.Sprintf("ZmspxFile('%s': %d textures, %d frames, %d animations)",
		z.Filepath, z.NumTextures, z.NumFrames, z.NumAnimations)
}

// baseName 取文件名并去掉扩展名 (对应 Python os.path.splitext(os.path.basename(p))[0])
func baseName(p string) string {
	return strings.TrimSuffix(filepath.Base(p), filepath.Ext(filepath.Base(p)))
}

// writeMetadataJSON 将元数据以 JSON 形式写入文件 (与 Python json.dump 格式一致)
func writeMetadataJSON(path string, z *ZmspxFile) error {
	b, err := json.MarshalIndent(z.ToDict(), "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, b, 0644)
}

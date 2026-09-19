package main

// ZMSPX (zms2) 文件格式解析库 —— 修正版
//
// 相对原版的修正（详见 zmspx_format_report.md）:
//   1. 帧记录前 4 字节是两个 u16 (nk, nx)，原版把前 4 字节当成一个 u32 当 nk，导致关键帧数量巨大、
//      后面多读出越界条目 (例如 fish_shark 的 textures 变成 [0 65445 65456])。
//   2. 帧记录的 +8/+10/+12/+14 是 bbox (s16 minX,minY,maxX,maxY)，画布尺寸 = (maxX-minX)×(maxY-minY)。
//      原版把 +12/+14 当成画布宽高且按 u16 读，得到的是"右下角坐标"，画布被截掉一半
//      (fish_shark 186x83 被读成 93x41)。
//   3. 关键帧条目是 8 字节: texIndex(u16) x(s16) y(s16) flags(u16)。
//      原版 x 按 u16 读、y 取 kfOff+4 的 u32（把 x、y 连读成一个 dword），且完全忽略 flags。
//   4. 关键帧 flags: bit0 = 上下翻转, bit1 = 左右翻转 (bit2 为其它标志，本样例中不改变变换)。
//      贴图位置 = (x-minX, y-minY)。
//   5. 像素格式 = type & 0xFF: 1 = 16bpp RGB565(小端)，3 = 32bpp BGRA8888(字节序 B,G,R,A)。
//      原版把 3 当 RGBA(导致 R/B 互换)、把 257 当 LA88 灰度(实际是 16bpp RGB565 且自带 bit8 标志)。

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const (
	HeaderSize        = 28
	TextureRecordSize = 12
	FrameRecordSize   = 28
	AnimRecordSize    = 8
	KeyframeEntrySize = 8
	AuxRectEntrySize  = 8
)

// PixelFormat 像素类型映射: (type & 0xFF) -> 每像素字节数等
type PixelFormat struct {
	BPP  int
	Mode string
	Desc string
}

var pixelFormats = map[uint16]PixelFormat{
	1: {2, "RGB565", "16bpp RGB565 小端 (不透明, 16 位字按 0xRRRRRGGGGGGBBBBB 展开)"},
	2: {1, "PAL8", "8bpp 调色板索引 (本样例未出现, 未验证)"},
	3: {4, "BGRA", "32bpp BGRA8888 (字节序 B,G,R,A; 等价于 LE 0xAARRGGBB)"},
}

// Texture 单个纹理的信息
type Texture struct {
	Index      int
	Width      int
	Height     int
	Type       int // 完整 type 字段 (含高位标志, 如 0x101)
	BPP        int // 每像素字节数
	DataOffset int // 像素数据在文件中的偏移量
	DataSize   int // 像素数据大小
}

// PixelMode 返回像素模式名 (按 type & 0xFF 判定)
func (t *Texture) PixelMode() string {
	if f, ok := pixelFormats[uint16(t.Type)&0xFF]; ok {
		return f.Mode
	}
	return "BGRA"
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
	Flags        int // 取向标志: bit0 = 上下翻转, bit1 = 左右翻转
}

// FlipH 左右翻转 (bit1)
func (k Keyframe) FlipH() bool { return k.Flags&2 != 0 }

// FlipV 上下翻转 (bit0)
func (k Keyframe) FlipV() bool { return k.Flags&1 != 0 }

// AuxRect 帧附加矩形 (游戏逻辑用: 命中盒/裁剪盒), 渲染不使用
type AuxRect struct {
	X, Y, W, H int
}

// Frame 一帧画面: 在画布上组合多个纹理
type Frame struct {
	Index        int
	NumKeyframes int
	NumAuxRects  int
	X0, Y0       int // bbox 左上角
	X1, Y1       int // bbox 右下角
	CanvasWidth  int // = X1 - X0
	CanvasHeight int // = Y1 - Y0
	Keyframes    []Keyframe
	AuxRects     []AuxRect
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

// parseZmspx 解析 .zmspx 文件
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
		if f, ok := pixelFormats[uint16(typ)&0xFF]; ok {
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
			// 最后一张纹理的结束位置 = 帧 0 关键帧数组的起始 (帧表 +16)
			trailingStart := z.FileSize
			if z.NumFrames > 0 {
				frameTableOff := texTableOffset + z.NumTextures*TextureRecordSize
				if frameTableOff+20 <= len(data) {
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
		numKf := int(binary.LittleEndian.Uint16(data[off:]))       // u16, 不是 u32!
		numAux := int(binary.LittleEndian.Uint16(data[off+2:]))    // 附加矩形数量
		x0 := int(int16(binary.LittleEndian.Uint16(data[off+8:])))  // s16
		y0 := int(int16(binary.LittleEndian.Uint16(data[off+10:]))) // s16
		x1 := int(int16(binary.LittleEndian.Uint16(data[off+12:]))) // s16
		y1 := int(int16(binary.LittleEndian.Uint16(data[off+14:]))) // s16
		kfOff1 := int(binary.LittleEndian.Uint32(data[off+16:]))
		auxOff := int(binary.LittleEndian.Uint32(data[off+20:]))

		keyframes := []Keyframe{}
		for j := 0; j < numKf; j++ {
			kfOff := kfOff1 + j*KeyframeEntrySize
			if kfOff+KeyframeEntrySize > len(data) {
				break // 越界保护，防止崩溃
			}
			texIdx := int(binary.LittleEndian.Uint16(data[kfOff:]))
			x := int(int16(binary.LittleEndian.Uint16(data[kfOff+2:]))) // s16
			y := int(int16(binary.LittleEndian.Uint16(data[kfOff+4:]))) // s16 (原版误读 u32)
			flags := int(binary.LittleEndian.Uint16(data[kfOff+6:]))
			keyframes = append(keyframes, Keyframe{
				TextureIndex: texIdx, X: x, Y: y, Flags: flags,
			})
		}

		auxRects := []AuxRect{}
		for j := 0; j < numAux; j++ {
			a := auxOff + j*AuxRectEntrySize
			if a+AuxRectEntrySize > len(data) {
				break
			}
			auxRects = append(auxRects, AuxRect{
				X: int(int16(binary.LittleEndian.Uint16(data[a:]))),
				Y: int(int16(binary.LittleEndian.Uint16(data[a+2:]))),
				W: int(int16(binary.LittleEndian.Uint16(data[a+4:]))),
				H: int(int16(binary.LittleEndian.Uint16(data[a+6:]))),
			})
		}

		z.Frames = append(z.Frames, Frame{
			Index: i, NumKeyframes: numKf, NumAuxRects: numAux,
			X0: x0, Y0: y0, X1: x1, Y1: y1,
			CanvasWidth: x1 - x0, CanvasHeight: y1 - y0,
			Keyframes: keyframes, AuxRects: auxRects,
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

// ── JSON 序列化 ──

type textureDict struct {
	Index      int    `json:"index"`
	Width      int    `json:"width"`
	Height     int    `json:"height"`
	Type       int    `json:"type"`
	Mode       string `json:"mode"`
	BPP        int    `json:"bpp"`
	DataOffset int    `json:"data_offset"`
	DataSize   int    `json:"data_size"`
}

type keyframeDict struct {
	TextureIndex int  `json:"texture_index"`
	X            int  `json:"x"`
	Y            int  `json:"y"`
	Flags        int  `json:"flags"`
	FlipH        bool `json:"flip_h"`
	FlipV        bool `json:"flip_v"`
}

type auxRectDict struct {
	X int `json:"x"`
	Y int `json:"y"`
	W int `json:"w"`
	H int `json:"h"`
}

type frameDict struct {
	Index        int            `json:"index"`
	NumKeyframes int            `json:"num_keyframes"`
	NumAuxRects  int            `json:"num_aux_rects"`
	X0           int            `json:"x0"`
	Y0           int            `json:"y0"`
	X1           int            `json:"x1"`
	Y1           int            `json:"y1"`
	CanvasWidth  int            `json:"canvas_width"`
	CanvasHeight int            `json:"canvas_height"`
	Keyframes    []keyframeDict `json:"keyframes"`
	AuxRects     []auxRectDict  `json:"aux_rects"`
}

type animDict struct {
	Index         int   `json:"index"`
	DataSize      int   `json:"data_size"`
	DataOffset    int   `json:"data_offset"`
	FrameSequence []int `json:"frame_sequence"`
}

type zmspxDict struct {
	Magic         string        `json:"magic"`
	NumTextures   int           `json:"num_textures"`
	NumFrames     int           `json:"num_frames"`
	NumAnimations int           `json:"num_animations"`
	FileSize      int           `json:"file_size"`
	Textures      []textureDict `json:"textures"`
	Frames        []frameDict   `json:"frames"`
	Animations    []animDict    `json:"animations"`
}

// ToDict 转换为可序列化的结构 (用于 JSON 导出)
// 注: 所有切片初始化为空切片 (而非 nil)，确保空列表序列化为 [] 而非 null
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
			Type: t.Type, Mode: t.PixelMode(), BPP: t.BPP,
			DataOffset: t.DataOffset, DataSize: t.DataSize,
		})
	}
	for _, f := range z.Frames {
		fd := frameDict{
			Index: f.Index, NumKeyframes: f.NumKeyframes, NumAuxRects: f.NumAuxRects,
			X0: f.X0, Y0: f.Y0, X1: f.X1, Y1: f.Y1,
			CanvasWidth: f.CanvasWidth, CanvasHeight: f.CanvasHeight,
			Keyframes: []keyframeDict{}, AuxRects: []auxRectDict{},
		}
		for _, kf := range f.Keyframes {
			fd.Keyframes = append(fd.Keyframes, keyframeDict{
				TextureIndex: kf.TextureIndex, X: kf.X, Y: kf.Y,
				Flags: kf.Flags, FlipH: kf.FlipH(), FlipV: kf.FlipV(),
			})
		}
		for _, a := range f.AuxRects {
			fd.AuxRects = append(fd.AuxRects, auxRectDict{X: a.X, Y: a.Y, W: a.W, H: a.H})
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

// String 返回摘要
func (z *ZmspxFile) String() string {
	return fmt.Sprintf("ZmspxFile('%s': %d textures, %d frames, %d animations)",
		z.Filepath, z.NumTextures, z.NumFrames, z.NumAnimations)
}

// baseName 取文件名并去掉扩展名
func baseName(p string) string {
	return strings.TrimSuffix(filepath.Base(p), filepath.Ext(filepath.Base(p)))
}

// writeMetadataJSON 将元数据以 JSON 形式写入文件
func writeMetadataJSON(path string, z *ZmspxFile) error {
	b, err := json.MarshalIndent(z.ToDict(), "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, b, 0644)
}

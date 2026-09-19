package main

// app_header.go - 解析 .app 文件头（392 字节）。
// 解析逻辑直接复用 go_tools/applet_header_tool/applet_header.go
// （字段布局以 src/tool/paser_info.h 的 AppletHeader 为准），以便 applet_scan
// 无需依赖外部二进制即可 "调用头解析工具" 的能力。
//
// 注意：这里的结构体是**解析后的结果表示**（按偏移读出再转主机字节序），
// 不是内存映射的 overlay，字段偏移与 C 侧的 packed 布局并不相同。

import (
	"bytes"
	"encoding/binary"
	"fmt"
)

// headerSize .app 头部固定大小 (392 字节)
const headerSize = 0x188

// appletHeader .app 文件头结构（小端序，多字节整数已转换为主机字节序）
type appletHeader struct {
	AppletID          uint32 // 0x000 游戏 ID
	Version           uint32 // 0x004 游戏版本号
	Flags             uint32 // 0x008 标志位（可能用于区分软件/游戏类，具体未知）
	PayloadSize       uint32 // 0x00C 汇编程序大小，0x188 + 此值 = 文件大小
	Reserved0x010     uint32 // 0x010 留空
	AppName           string // 0x014 软件名称 (UTF-8, 32 字节)
	IconName          string // 0x034 软件图标名称 (32 字节，通常 "icon.zbmp")
	Reserved0x054     []byte // 0x054 留空 (32 字节)
	ProgramUID        []byte // 0x074 程序唯一 ID（16 字节，加密算法未知）
	ActivationType    uint32 // 0x084 软件激活类型（如 3 元 / 2 元 / 免费激活）
	ActivationKey     []byte // 0x088 激活成功后生成的密钥（16 字节，算法未知）
	UnknownDataLength uint32 // 0x098 从 0x09C 开始的数据长度
	UnknownData       []byte // 0x09C 未知数据区（固定 216 字节，激活后可能生成）
	MinScreenWidth    uint32 // 0x174 支持的最小屏幕宽度
	MinScreenHeight   uint32 // 0x178 支持的最小屏幕高度
	MaxScreenWidth    uint32 // 0x17C 支持的最大屏幕宽度
	MaxScreenHeight   uint32 // 0x180 支持的最大屏幕高度
	Unknown0x184      uint32 // 0x184 未知（可能和程序启动方式有关）
}

// parseAppHeader 从 392 字节原始数据中解析 applet 头
func parseAppHeader(raw []byte) (*appletHeader, error) {
	if len(raw) < headerSize {
		return nil, fmt.Errorf("头部不完整：需要 %d 字节，实际 %d", headerSize, len(raw))
	}
	h := &appletHeader{
		AppletID:          binary.LittleEndian.Uint32(raw[0x000:]),
		Version:           binary.LittleEndian.Uint32(raw[0x004:]),
		Flags:             binary.LittleEndian.Uint32(raw[0x008:]),
		PayloadSize:       binary.LittleEndian.Uint32(raw[0x00C:]),
		Reserved0x010:     binary.LittleEndian.Uint32(raw[0x010:]),
		AppName:           cString(raw[0x014 : 0x014+32]),
		IconName:          cString(raw[0x034 : 0x034+32]),
		Reserved0x054:     append([]byte{}, raw[0x054:0x054+32]...),
		ProgramUID:        append([]byte{}, raw[0x074:0x074+16]...),
		ActivationType:    binary.LittleEndian.Uint32(raw[0x084:]),
		ActivationKey:     append([]byte{}, raw[0x088:0x088+16]...),
		UnknownDataLength: binary.LittleEndian.Uint32(raw[0x098:]),
		UnknownData:       append([]byte{}, raw[0x09C:0x09C+216]...),
		MinScreenWidth:    binary.LittleEndian.Uint32(raw[0x174:]),
		MinScreenHeight:   binary.LittleEndian.Uint32(raw[0x178:]),
		MaxScreenWidth:    binary.LittleEndian.Uint32(raw[0x17C:]),
		MaxScreenHeight:   binary.LittleEndian.Uint32(raw[0x180:]),
		Unknown0x184:      binary.LittleEndian.Uint32(raw[0x184:]),
	}
	return h, nil
}

// cString 将固定长度缓冲区中 0 终止的字符串提取出来
func cString(b []byte) string {
	if i := bytes.IndexByte(b, 0); i >= 0 {
		b = b[:i]
	}
	return string(b)
}

// hexBytes 把字节块格式化成 "XX XX ..." 的十六进制串（CSV 用）
func hexBytes(b []byte) string {
	var sb bytes.Buffer
	for i, v := range b {
		if i > 0 {
			sb.WriteByte(' ')
		}
		fmt.Fprintf(&sb, "%02X", v)
	}
	return sb.String()
}

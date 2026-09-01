package main

// applet_header.go - 解析 .app 文件头（392 字节）。
// 实现参考 src/tool/paser_info.c。

import (
	"bytes"
	"encoding/binary"
	"fmt"
)

// headerSize .app 头部固定大小 (392 字节)
const headerSize = 0x188

// appletHeader .app 文件头结构（小端序，多字节整数已转换为主机字节序）
type appletHeader struct {
	AppletID       uint32 // 0x000 AppletID
	Version        uint32 // 0x004 版本号
	Flags          uint32 // 0x008 标志位（已观察 0x02/0x10/0x20）
	PayloadSize    uint32 // 0x00C 负载大小，0x188 + 此值 = 文件大小
	Reserved0x010  uint32 // 0x010 保留字段
	AppName        string // 0x014 应用名称 (UTF-8, 32 字节)
	IconName       string // 0x034 图标文件名 (ASCII, 32 字节)
	Reserved0x054  []byte // 0x054 保留字段 (32 字节)
	Type           uint8  // 0x074 类型（单字节）
	SignatureData  []byte // 0x075 签名/校验数据 (15 字节)
	Unknown0x084   uint32 // 0x084 未知字段
	Extended       []byte // 0x088 保留/扩展数据区 (236 字节)
	ScreenWSmall   uint32 // 0x174 小尺寸屏幕宽度
	ScreenHSmall   uint32 // 0x178 小尺寸屏幕高度
	ScreenW        uint32 // 0x17C 大尺寸屏幕宽度
	ScreenH        uint32 // 0x180 大尺寸屏幕高度
	Unknown0x184   uint32 // 0x184 未知字段（高16位/低16位两个子字段）
}

// parseAppHeader 从 392 字节原始数据中解析 applet 头
func parseAppHeader(raw []byte) (*appletHeader, error) {
	if len(raw) < headerSize {
		return nil, fmt.Errorf("头部不完整：需要 %d 字节，实际 %d", headerSize, len(raw))
	}
	h := &appletHeader{
		AppletID:      binary.LittleEndian.Uint32(raw[0x000:]),
		Version:       binary.LittleEndian.Uint32(raw[0x004:]),
		Flags:         binary.LittleEndian.Uint32(raw[0x008:]),
		PayloadSize:   binary.LittleEndian.Uint32(raw[0x00C:]),
		Reserved0x010: binary.LittleEndian.Uint32(raw[0x010:]),
		AppName:       cString(raw[0x014 : 0x014+32]),
		IconName:      cString(raw[0x034 : 0x034+32]),
		Reserved0x054: append([]byte{}, raw[0x054:0x054+32]...),
		Type:          raw[0x074],
		SignatureData: append([]byte{}, raw[0x075:0x075+15]...),
		Unknown0x084:  binary.LittleEndian.Uint32(raw[0x084:]),
		Extended:      append([]byte{}, raw[0x088:0x088+236]...),
		ScreenWSmall:  binary.LittleEndian.Uint32(raw[0x174:]),
		ScreenHSmall:  binary.LittleEndian.Uint32(raw[0x178:]),
		ScreenW:       binary.LittleEndian.Uint32(raw[0x17C:]),
		ScreenH:       binary.LittleEndian.Uint32(raw[0x180:]),
		Unknown0x184:  binary.LittleEndian.Uint32(raw[0x184:]),
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

// print 输出所有头部字段（与 paser_info.c 的 print_header 一致）
func (h *appletHeader) print() {
	fmt.Println("========== .app 头部信息 ==========")
	fmt.Printf("AppletID        : 0x%08X (%d)\n", h.AppletID, h.AppletID)
	fmt.Printf("Version         : 0x%08X (%d)\n", h.Version, h.Version)
	fmt.Printf("Flags           : 0x%08X\n", h.Flags)
	fmt.Printf("PayloadSize     : 0x%08X (%d 字节)\n", h.PayloadSize, h.PayloadSize)
	fmt.Printf("Reserved_0x010  : 0x%08X\n", h.Reserved0x010)
	fmt.Printf("AppName         : \"%s\"\n", h.AppName)
	fmt.Printf("IconName        : \"%s\"\n", h.IconName)
	fmt.Printf("Type            : 0x%02X\n", h.Type)
	fmt.Printf("SignatureData   : ")
	for _, b := range h.SignatureData {
		fmt.Printf("%02X ", b)
	}
	fmt.Println()
	fmt.Printf("Unknown_0x084   : 0x%08X\n", h.Unknown0x084)
	fmt.Printf("ScreenW_Small(0x174)   : 0x%08X (%d)\n", h.ScreenWSmall, h.ScreenWSmall)
	fmt.Printf("ScreenH_Small(0x178)   : 0x%08X (%d)\n", h.ScreenHSmall, h.ScreenHSmall)
	fmt.Printf("ScreenW (0x17C) : 0x%08X (%d px)\n", h.ScreenW, h.ScreenW)
	fmt.Printf("ScreenH (0x180) : 0x%08X (%d px)\n", h.ScreenH, h.ScreenH)
	fmt.Printf("Unknown_0x184   : 0x%08X (高16位:0x%04X, 低16位:0x%04X)\n",
		h.Unknown0x184, (h.Unknown0x184>>16)&0xFFFF, h.Unknown0x184&0xFFFF)
	fmt.Println("====================================")
}

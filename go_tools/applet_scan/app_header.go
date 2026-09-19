package main

// app_header.go - 解析 .app 文件头（392 字节）。
// 解析逻辑直接复用 go_tools/applet_header_tool/applet_header.go
// （实现参考 src/tool/paser_info.c），以便 applet_scan 无需依赖外部二进制即可
// "调用头解析工具" 的能力。

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

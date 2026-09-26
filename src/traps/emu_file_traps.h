#ifndef EMU_FILE_TRAPS_H
#define EMU_FILE_TRAPS_H

#include "../emu_mem_layout.h"

/* -------------------- FILE_VT_ADDR 枚举 -------------------- */
/* ZMAEE IFile 虚表槽位（基址 FILE_VT_ADDR） */
enum ZM_FILE_VT : uint32_t {
	ZM_File_AddRef = 0x00U,
	ZM_File_Release = 0x04U,
	ZM_File_Read = 0x08U,
	ZM_File_Write = 0x0CU,
	ZM_File_Readable = 0x10U,
	ZM_File_Writeable = 0x14U,
	ZM_File_Cancel = 0x18U,
	ZM_File_Flush = 0x1CU,
	ZM_File_Seek = 0x20U,
	ZM_File_Tell = 0x24U
};

/* -------------------- FileMgr_VT_ADDR 枚举 -------------------- */
/* ZMAEE IFileMgr 虚表槽位（基址 FileMgr_VT_ADDR，16 槽） */
enum ZM_FILEMGR_VT : uint32_t {
	ZM_FileMgr_AddRef = 0x00U,
	ZM_FileMgr_Release = 0x04U,
	ZM_FileMgr_OpenFile = 0x08U,
	ZM_FileMgr_x0C = 0x0CU,
	ZM_FileMgr_x10 = 0x10U,
	ZM_FileMgr_x14 = 0x14U,
	ZM_FileMgr_x18 = 0x18U,
	ZM_FileMgr_x1C = 0x1CU,
	ZM_FileMgr_x20 = 0x20U,
	ZM_FileMgr_x24 = 0x24U,
	ZM_FileMgr_x28 = 0x28U,
	ZM_FileMgr_x2C = 0x2CU,
	ZM_FileMgr_x30 = 0x30U,
	ZM_FileMgr_x34 = 0x34U,
	ZM_FileMgr_x38 = 0x38U,
	/* +0x3C：00000001 实测会用（sub_68D8 @0x68F4 读 [vt+0x3C] 后 BLX）：
	 *   R0 = IFileMgr 对象、R1 = 对象+0xC 的值、R2 = &sub_68D8（回调）、R3 = 0
	 *   返回值被写进 UI 对象的 +0x10，之后该类的 getter（类表+0x20 → 0xD054 的
	 *   LDR R0,[R0,#0x10]）把它当 this 用 —— 之前这格不存在，读表外 → 返回 0
	 *   → NULL 当 this → 崩在 pc=0x1EDC。语义待从参数/回调确认后再实现。 */
	ZM_FileMgr_x3C = 0x3CU
};

/* fs */
/*
 * IFile 虚表（逆向实测：gAEEFileVtbl @ .data:00064010，函数指针均 +1 表示
 * Thumb）
 *
 *   +0x00  ZMAEE_IFile_AddRef
 *   +0x04  ZMAEE_IFile_Release   ← 本模拟器的 file.close
 *   +0x08  ZMAEE_IFile_Read      ← 本模拟器的 file.read
 *   +0x0C  ZMAEE_IFile_Write     （未接线）
 *   +0x10  ZMAEE_IFile_Readable  （未接线）
 *   +0x14  ZMAEE_IFile_Writeable （未接线）
 *   +0x18  ZMAEE_IFile_Cancel    （未接线）
 *   +0x1C  ZMAEE_IFile_Flush     （未接线）
 *   +0x20  ZMAEE_IFile_Seek      ← 本模拟器的 file.seek
 *   +0x24  ZMAEE_IFile_Tell      ← 本模拟器的 file.tell
 *
 * 关键点：表里**没有** GetSize/Size 槽位。取文件大小的惯用法只能是
 *   Seek(0, SEEK_END) 然后 Tell()（此时位置恰好等于总大小）。
 * 因此 +0x24 必须实现为 Tell（返回当前读写位置）。
 * 之前实现成"返回总大小"是错的 —— 只在"先 seek 到末尾"这一种调用序列下
 * 碰巧正确，在任意位置调用会给出错误结果。
 *
 * 注：seek 的参数序（whence/offset 谁在前）尚无 applet 覆盖验证，
 * 保持现状未改动；若后续有 applet 用到 seek，需用 RE 数据核对。
 */
enum ZM_FILE_TRAPS : uint32_t {
	TR_file_close = TRAP(FILE_VT_ADDR + ZM_File_Release), /* Release */
	TR_file_read = TRAP(FILE_VT_ADDR + ZM_File_Read),
	TR_file_write = TRAP(FILE_VT_ADDR + ZM_File_Write),
	TR_file_seek = TRAP(FILE_VT_ADDR + ZM_File_Seek),
	TR_file_tell = TRAP(FILE_VT_ADDR + ZM_File_Tell) /* Tell：返回当前读写位置 */
};

/* ---- ZMAEE IFileMgr 原生虚表（RE 实测：g_filemgr_vtbl @ .data:00064038，
 * 16 槽；紧随 gAEEFileVtbl @0x64010 之后）----
 *   +0x00 sub_29DE8  +0x04 sub_29DF0   （惯例 AddRef/Release）
 *   +0x08 ZMAEE_IFileMgr_OpenFile
 *   +0x0C sub_2A550  +0x10 sub_2A4E0  +0x14 sub_2A45C  +0x18 sub_2A3F4
 *   +0x1C sub_2A344  +0x20 sub_2A7BC  +0x24 sub_2A2D0  +0x28 sub_29EA0
 *   +0x2C sub_29E7C  +0x30 sub_29E40  +0x34 sub_29E08  +0x38 sub_29E00
 * 注：+0x30 RE 已证伪"enumFile"旧说——实为存储区支持查询
 * （a2: 0→'C'内置盘，1→'E'，>=2→SD 挂载?'T':0）。
 * +0x20 RE=sub_2A7BC：TestFile 存在性检查（ConvertFileName 分派
 * 包内/ assets.zip / 文件系统三路），非目录枚举；枚举槽待 RE。 */
enum ZM_FILEMGR_TRAPS : uint32_t {
	TR_fileMgr_AddRef = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_AddRef),
	TR_fileMgr_Release = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_Release),
	TR_fileMgr_open_file = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_OpenFile),
	TR_fileMgr_x3C = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x3C),
	TR_fileMgr_x0C = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x0C), /* RE sub_2A550 */
	TR_fileMgr_x10 = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x10), /* RE sub_2A4E0 */
	TR_fileMgr_x14 = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x14), /* RE sub_2A45C */
	TR_fileMgr_x18 = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x18), /* RE sub_2A3F4 */
	TR_fileMgr_x1C = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x1C), /* RE sub_2A344 */
	TR_fileMgr_x20 = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x20), /* RE sub_2A7BC（00001b62 高频） */
	TR_fileMgr_x24 = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x24), /* RE sub_2A2D0 */
	TR_fileMgr_x28 = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x28), /* RE sub_29EA0 */
	TR_fileMgr_x2C = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x2C), /* RE sub_29E7C */
	TR_fileMgr_x30 = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x30), /* RE sub_29E40（旧称 enumFile） */
	TR_fileMgr_x34 = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x34), /* RE sub_29E08 */
	TR_fileMgr_x38 = TRAP(FileMgr_VT_ADDR + ZM_FileMgr_x38)	 /* RE sub_29E00 */
};

#endif /* EMU_FILE_TRAPS_H */

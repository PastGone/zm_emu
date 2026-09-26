#ifndef ZM_CBK_FILE_H
#define ZM_CBK_FILE_H

#include <stdint.h>
#include <unicorn/unicorn.h>

/* CBK "文件对象"（资源工厂那条路，见 emu_mem_regions.h 的 CBK_TRAP_BASE）。
 *
 * 背景：有一族 applet（0000050b《新还猪格格》、00001b63 等）把 [CBK_OBJ+0x4C]
 * （分配器）当"带虚表的对象"用，并把它的 [+0]→[+8] 当**打开资源文件**的工厂：
 *   拼出 "<目录>\res\game%d.ypak"      （实测字符串 ✓）
 *   obj  = [管理器+8](this, 路径, 1)    ← 本模块的 zm_cbk_file_open
 *   size = obj->vt[0x24]()             ← 真机 = 资源包大小
 *   buf  = malloc(size)
 *   obj->vt[0x08](buf, size)           ← 读内容
 *   obj->vt[0x04]()                    ← 释放
 * 以前 [+8] 是一段"只会返回假对象"的桩 ✗ → size 拿到的是**指针**（0xFD0300）
 * → malloc(16MB) 失败 → NULL → 之后把"尺寸值"当指针用 → 画到映射外 err=7 ✗。
 *
 * 现在按**文件**语义实现（size/read/write/seek/release），对象造在 CBK 跳板页里，
 * 虚表槽指向 CBK_TRAP_BASE+... 的陷阱（见 trap.c 的表）。 */

uint32_t zm_cbk_file_open(uc_engine *uc, uint32_t path_ptr, uint32_t mode);
uint32_t zm_cbk_file_size(uc_engine *uc, uint32_t obj);
uint32_t zm_cbk_file_read(uc_engine *uc, uint32_t obj, uint32_t buf, uint32_t len);
uint32_t zm_cbk_file_write(uc_engine *uc, uint32_t obj, uint32_t buf, uint32_t len);
uint32_t zm_cbk_file_seek(uc_engine *uc, uint32_t obj, uint32_t whence, uint32_t off);
uint32_t zm_cbk_file_release(uc_engine *uc, uint32_t obj);

/**
 * @brief 退出前的兜底刷盘：把"写过但没 release"的文件也写回宿主。
 *
 * 存档（这一族就是"打开/新建 → 写 → 关闭"）如果在 release 前就退出，改动会丢；
 * 由 main.c 的退出路径调用。只刷 **dirty**（真被写过）的文件，纯读的资源包不碰。
 */
void zm_cbk_file_flush_all(void);

#endif /* ZM_CBK_FILE_H */

/**
 * @file trap_handlers_io.c
 * @brief IFileMgr / IFile / CBK 文件对象的槽位 handler
 */

#include "../emu.h"
#include "../log/log.h"
#include "../zmaee/fs/zm_cbk_file.h" /* CBK 文件对象（跳板页陷阱窗口） */
#include "../zmaee/fs/zm_file.h"
#include "../zmaee/fs/zm_file_mgr.h"
#include "trap_internal.h"

/* ==========================================================================
 * IFileMgr
 * ========================================================================== */

uint32_t a_zm_fileMgr_GetFreeSize(trap_ctx *c) {
	return zm_fileMgr_GetFreeSize(c->uc, c->r0, c->r1);
}

uint32_t a_zm_fileMgr_GetInfo(trap_ctx *c) {
	return zm_fileMgr_GetInfo(c->uc, c->r0, c->r1, c->r2);
}

uint32_t a_zm_fileMgr_TestFile(trap_ctx *c) {
	return zm_fileMgr_TestFile(c->uc, c->r0, c->r1);
}

uint32_t a_zm_fileMgr_StorageSupport(trap_ctx *c) {
	return zm_fileMgr_StorageSupport(c->uc, c->r0, c->r1);
}

/* +0x14 mkdir（安卓 sub_19720 / 手机版 0x111AC 双向确认）：
 * applet 读写数据文件前会逐级建目录，配合 OpenFile 的"新建空文件"
 * 才能让 0000042f 的存档（OpenFile→Write(8B)→Close）真正落盘。 */
uint32_t a_zm_fileMgr_make_dir(trap_ctx *c) {
	/* 与 open_file 同约定：r0 = this，r1 = 路径 */
	return zm_fileMgr_make_dir(c->uc, c->r1);
}

uint32_t a_fileMgr_open_file(trap_ctx *c) {
	if (c->r0 == 0)
		return 0;
	return zm_fileMgr_open_file(c->uc, c->r1);
}

uint32_t a_fileMgr_x3C(trap_ctx *c) {
	log_debug("[FileMgr+0x3C] r0=0x%X r1=0x%X r2=0x%X r3=0x%X lr=0x%X",
			  c->r0,
			  c->r1,
			  c->r2,
			  c->r3,
			  c->lr);
	return zm_fileMgr_stub(c->uc, 0x3C, c->r0, c->r1, c->r2, c->r3);
}

/* 其余未实现的 IFileMgr 槽：偏移由表项提供 */
uint32_t a_fileMgr_stub(trap_ctx *c) {
	return zm_fileMgr_stub(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}

/* ==========================================================================
 * IFile
 * ========================================================================== */

uint32_t a_zm_file_close(trap_ctx *c) {
	return zm_file_close(c->uc, c->r0);
}

uint32_t a_zm_file_read(trap_ctx *c) {
	return zm_file_read(c->uc, c->r0, c->r1, c->r2);
}

uint32_t a_zm_file_write(trap_ctx *c) {
	return zm_file_write(c->uc, c->r0, c->r1, c->r2);
}

uint32_t a_zm_file_seek(trap_ctx *c) {
	return zm_file_seek(c->uc, c->r0, c->r1, c->r2);
}

uint32_t a_zm_file_tell(trap_ctx *c) {
	return zm_file_tell(c->uc, c->r0);
}

/* ==========================================================================
 * CBK 文件对象陷阱（跳板页窗口，见 emu_mem_regions.h 与 zm_cbk_file.h）
 *
 * 这一族 applet 把 [CBK_OBJ+0x4C]→[+0] 的 +8 当"打开资源文件"的工厂用
 * （实测参数：r1 = "<目录>\res\gameN.ypak" 的字符串，r2 = 1）。以前那里是
 * "只会返回假对象"的桩 ✗ → 它把假对象地址当"文件大小"去 malloc(16MB) → 失败
 * → NULL → 后面把尺寸当指针 → 画到映射外崩。现在按真文件办事 ✓。
 * ========================================================================== */

uint32_t a_cbk_open_file(trap_ctx *c) {
	return zm_cbk_file_open(c->uc, c->r1, c->r2);
}

uint32_t a_cbk_file_release(trap_ctx *c) {
	return zm_cbk_file_release(c->uc, c->r0);
}

uint32_t a_cbk_file_read(trap_ctx *c) {
	return zm_cbk_file_read(c->uc, c->r0, c->r1, c->r2);
}

uint32_t a_cbk_file_write(trap_ctx *c) {
	return zm_cbk_file_write(c->uc, c->r0, c->r1, c->r2);
}

uint32_t a_cbk_file_seek(trap_ctx *c) {
	return zm_cbk_file_seek(c->uc, c->r0, c->r1, c->r2);
}

uint32_t a_cbk_file_size(trap_ctx *c) {
	return zm_cbk_file_size(c->uc, c->r0);
}

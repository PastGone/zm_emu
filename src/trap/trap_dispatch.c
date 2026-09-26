/**
 * @file trap_dispatch.c
 * @brief 陷阱分派：分派表 + 查表 + 兜底 + handle_trap
 *
 * 这是整个 trap 子系统里唯一"知道所有槽"的文件。handler 的实现散在
 * trap_handlers_*.c，想找"这个槽做什么"看那边；想知道"这个槽指向谁"
 * 看本文件的 k_trap_table。
 *
 * 表项四列：槽区间 / handler / 字面量 off / kind（是否接管控制流）。
 * 注意：tapi / surf / image 的精确项必须排在各自区间之前，保证优先匹配。
 */

#include <inttypes.h> /* PRIx32 */
#include <stdlib.h>	  /* getenv */

#include "../emu.h"
#include "../log/log.h"
#include "../tool/uc_helper.h" /* uc_read32 */
#include "trap_internal.h"

/* =========================================================================
 * 分派表
 *
 * 旧实现是 1100+ 行的巨型 switch：手写分派既长，又容易出复制粘贴缺陷
 * （例如曾把一段无 case 标签的 `ret = zm_shell_stub(...)` 落在
 * TR_shell_GetAppDir 之后、永远不可达——已删除）。改为“陷阱地址 → 处理函数”
 * 的分派表：新增一个槽位 = 表内加一行。
 * ========================================================================= */
static const trap_entry k_trap_table[] = {
	/* ---- 控制流 / 特殊 ---- */
	SLOT_PC(TR_timer_return, a_timer_return),
	SLOT_PC(TR_Applet_Internal_Reg_callback, a_reg_callback),
	SLOT(TR_register_event_loop, a_register_event_loop),
	SLOT_PC(TR_abort, a_abort),
	SLOT_PC(TR_enter_event_loop, a_enter_event_loop),
	SLOT(TR_root_start_timer, a_zm_timer_StartTimer),
	SLOT(TR_root_stop_timer, a_zm_timer_StopTimer),
	SLOT(TR_root_create_cbk, a_root_create_cbk),
	/* CBK 文件对象陷阱（在 CBK 跳板页的窗口里，不是 TRAMP 区）：
	 * 这一族把 [CBK+0x4C]→[+0] 的 +8 当"打开资源文件"用，见 zm_cbk_file.h。 */
	SLOT(CBK_TRAP_BASE + 0x00, a_cbk_open_file),
	SLOT(CBK_TRAP_BASE + 0x04, a_cbk_file_release),
	SLOT(CBK_TRAP_BASE + 0x08, a_cbk_file_read),
	SLOT(CBK_TRAP_BASE + 0x0C, a_cbk_file_write),
	SLOT(CBK_TRAP_BASE + 0x10, a_cbk_file_seek),
	SLOT(CBK_TRAP_BASE + 0x14, a_cbk_file_size),
	SLOT(TR_shell_CreateInstance, a_shell_CreateInstance),

	/* ---- root ---- */
	SLOT(TR_root_getShell, a_root_getShell),
	SLOT(TR_root_malloc, a_root_malloc),
	SLOT(TR_root_free, a_root_free),
	SLOT(TR_root_malloc_screen, a_root_malloc_screen),
	SLOT(TR_root_free_screen, a_root_free_screen),
	SLOT(TR_root_x3C, a_root_x3C),
	SLOT(TR_root_ucs2_to_utf8, a_zm_ucs2_to_utf8),
	SLOT(TR_root_utf8_to_ucs2, a_zm_utf8_to_ucs2),
	SLOT(TR_root_sprintf, a_root_sprintf),
	SLOT(TR_root_str_to_num, a_root_str_to_num),
	SLOT(TR_root_atof, a_root_atof),
	SLOT(TR_root_x12C, a_root_f_op),
	SLOT(TR_root_str_ctor, a_root_str_ctor),
	SLOT(TR_root_strcmp, a_zm_strcmp),
	SLOT(TR_root_strchr, a_zm_strlen),
	SLOT(TR_root_memcmp, a_u_memcmp),
	SLOT(TR_root_memcpy, a_u_memcpy),
	SLOT(TR_root_memset, a_u_memset),
	SLOT(TR_root_str_assign, a_zm_root_str_assign),
	SLOT(TR_root_strstr, a_zm_strstr),
	SLOT(TR_root_x68C, a_zm_root_x68C),
	SLOT(TR_root_spec_lookup, a_zm_spec_lookup),
	SLOT(TR_root_str_find, a_zm_strchr),
	SLOT(TR_root_wcslen, a_zm_wcslen),
	SLOT(TR_root_srand, a_root_srand),
	SLOT(TR_root_rand, a_root_rand),
	SLOT(TR_root_sqrt, a_root_sqrt),
	SLOT(TR_root_cos, a_root_cos),
	SLOT(TR_root_sin, a_root_sin),
	SLOT(TR_root_atan, a_root_atan),
	SLOT(TR_root_tan, a_root_tan),

	/* ---- shell ---- */
	SLOT(TR_shell_AddRef, a_zm_shell_AddRef),
	SLOT(TR_shell_Release, a_zm_shell_Release),
	SLOT_OFF(TR_shell_x0C, a_shell_stub, 0x0C),
	SLOT(TR_shell_GetDeviceInfo, a_zm_shell_GetDeviceInfo),
	SLOT(TR_shell_GetRootDir, a_zm_shell_GetRootDir),
	SLOT_OFF(TR_shell_SetWorkDir, a_shell_stub, 0x18),
	SLOT(TR_shell_GetWorkDir, a_zm_shell_GetWorkDir),
	SLOT_OFF(TR_shell_StartApplet, a_shell_stub, 0x20),
	SLOT(TR_shell_CloseApplet, a_zm_shell_CloseApplet),
	SLOT_OFF(TR_shell_CanStartApplet, a_shell_stub, 0x28),
	SLOT_OFF(TR_shell_ActiveApplet, a_shell_stub, 0x2C),
	SLOT(TR_shell_GetApplet, a_zm_shell_GetApplet),
	SLOT_OFF(TR_shell_x34, a_shell_stub, 0x34),
	SLOT_OFF(TR_shell_x38, a_shell_stub, 0x38),
	SLOT(TR_shell_SetTimer, a_shell_SetTimer),
	SLOT(TR_shell_CancelTimer, a_zm_timer_CancelTimer),
	SLOT(TR_shell_CancelOwnerTimer, a_zm_timer_CancelOwnerTimer),
	SLOT(TR_shell_GetTickCount, a_zm_shell_GetTickCount),
	SLOT_OFF(TR_shell_OpenWapBrowser, a_shell_stub, 0x4C),
	SLOT_OFF(TR_shell_x50, a_shell_stub, 0x50),
	SLOT_OFF(TR_shell_SetEndKeyMask, a_shell_stub, 0x54),
	SLOT(TR_shell_LoadDLL, a_shell_LoadDLL),
	SLOT(TR_shell_UnloadDLL, a_zm_shell_UnloadDLL),
	SLOT_OFF(TR_shell_GetAppletMask, a_shell_stub, 0x60),
	SLOT_OFF(TR_shell_SetAppletMask, a_shell_stub, 0x64),
	SLOT_OFF(TR_shell_IsLoadGlobalLibrary, a_shell_stub, 0x68),
	SLOT_OFF(TR_shell_LoadGlobalLibrary, a_shell_stub, 0x6C),
	SLOT_OFF(TR_shell_FreeGlobalLibrary, a_shell_stub, 0x70),
	SLOT_OFF(TR_shell_IsGlobalLibraryUseStaticMem, a_shell_stub, 0x74),
	SLOT(TR_shell_LoadLibraryExt, a_zm_shell_LoadLibraryExt),
	SLOT_OFF(TR_shell_EntryApplet, a_shell_stub, 0x7C),
	SLOT(TR_shell_GetAppDir, a_shell_GetAppDir),
	SLOT_OFF(TR_shell_GetSupportHall, a_shell_stub, 0x84),

	/* ---- fileMgr / file ---- */
	SLOT_OFF(TR_fileMgr_AddRef, a_fileMgr_stub, 0x00),
	SLOT_OFF(TR_fileMgr_Release, a_fileMgr_stub, 0x04),
	SLOT(TR_fileMgr_open_file, a_fileMgr_open_file),
	SLOT(TR_fileMgr_x0C, a_zm_fileMgr_GetInfo),
	SLOT_OFF(TR_fileMgr_x10, a_fileMgr_stub, 0x10),
	/* +0x14 = mkdir（原为空桩）。applet 读写数据文件前会逐级建目录
	 * （安卓 sub_19720 / 手机版 0x111AC 双向确认），配合 OpenFile 的"新建空文件"
	 * 才能让 0000042f 的存档（OpenFile→Write(8B)→Close）真正落盘。 */
	SLOT(TR_fileMgr_x14, a_zm_fileMgr_make_dir),
	SLOT_OFF(TR_fileMgr_x18, a_fileMgr_stub, 0x18),
	SLOT_OFF(TR_fileMgr_x1C, a_fileMgr_stub, 0x1C),
	SLOT(TR_fileMgr_x20, a_zm_fileMgr_TestFile),
	SLOT_OFF(TR_fileMgr_x24, a_fileMgr_stub, 0x24),
	SLOT_OFF(TR_fileMgr_x28, a_fileMgr_stub, 0x28),
	SLOT_OFF(TR_fileMgr_x2C, a_fileMgr_stub, 0x2C),
	SLOT(TR_fileMgr_x30, a_zm_fileMgr_StorageSupport),
	/* +0x34 GetFreeSize（RE sub_29E08）：00000442 启动时靠它判断"空间够不够"；
	 * 之前是空桩恒返 0 → 永远判成"磁盘空间不足" */
	SLOT(TR_fileMgr_x34, a_zm_fileMgr_GetFreeSize),
	SLOT(TR_fileMgr_x38, a_fileMgr_x3C),
	SLOT(TR_file_close, a_zm_file_close),
	SLOT(TR_file_read, a_zm_file_read),
	SLOT(TR_file_write, a_zm_file_write),
	SLOT(TR_file_seek, a_zm_file_seek),
	SLOT(TR_file_tell, a_zm_file_tell),

	/* ---- util 区间 ---- */
	RANGE(TR_util_x00, TR_util_x18, d_util_stub),

	/* ---- display ---- */
	SLOT(TR_display_AddRef, a_zm_display_AddRef),
	SLOT(TR_display_Release, a_zm_display_Release),
	SLOT(TR_display_GetMaxLayerCount, a_zm_display_GetMaxLayerCount),
	SLOT_OFF(TR_display_CreateLayer, a_disp_CreateLayer, 0x0C),
	SLOT_OFF(TR_display_CreateLayerExt, a_disp_CreateLayerExt, 0x10),
	SLOT(TR_display_FreeLayer, a_zm_display_FreeLayer),
	SLOT(TR_display_FreeAllLayer, a_zm_display_FreeAllLayer),
	SLOT_OFF(TR_display_GetLayerInfo, a_disp_GetLayerInfo, 0x1C),
	SLOT(TR_display_SetActiveLayer, a_zm_display_SetActiveLayer),
	SLOT_OFF(TR_display_SetLayerPosition, a_disp_SetLayerPosition, 0x24),
	SLOT(TR_display_Update, a_display_Update),
	SLOT(TR_display_UpdateEx, a_zm_display_UpdateEx),
	SLOT(TR_display_GetActiveLayer, a_zm_display_GetActiveLayer),
	SLOT_OFF(TR_display_LockScreen, a_disp_LockScreen, 0x34),
	SLOT(TR_display_UnlockScreen, a_zm_display_UnlockScreen),
	SLOT_OFF(TR_display_RegisterCustomFont, a_disp_RegisterCustomFont, 0x3C),
	SLOT(TR_display_SelectFont, a_zm_display_SelectFont),
	SLOT(TR_display_GetFontWidth, a_zm_display_GetFontWidth),
	SLOT(TR_display_GetFontHeight, a_zm_display_GetFontHeight),
	SLOT(TR_display_MeasureString, a_display_MeasureString),
	SLOT(TR_display_DrawText, a_display_DrawText),
	SLOT(TR_display_SetTransColor, a_zm_display_SetTransColor),
	SLOT_OFF(TR_display_SetOpacity, a_disp_SetOpacity, 0x58),
	SLOT_OFF(TR_display_SetClipRect, a_disp_SetClipRect, 0x5C),
	SLOT_OFF(TR_display_GetClipRect, a_disp_GetClipRect, 0x60),
	SLOT_OFF(TR_display_SetPixel, a_disp_SetPixel, 0x64),
	SLOT_OFF(TR_display_DrawLine, a_disp_DrawLine, 0x68),
	SLOT(TR_display_DrawRect, a_display_DrawRect),
	SLOT(TR_display_FillRect, a_display_FillRect),
	SLOT_OFF(TR_display_DrawRoundRect, a_disp_DrawRoundRect, 0x74),
	SLOT_OFF(TR_display_DrawCircle, a_disp_DrawCircle, 0x78),
	SLOT_OFF(TR_display_FillCircle, a_disp_FillCircle, 0x7C),
	SLOT_OFF(TR_display_DrawArc, a_disp_DrawArc, 0x80),
	SLOT_OFF(TR_display_FillArc, a_disp_FillArc, 0x84),
	SLOT_OFF(TR_display_FillGradientRect, a_disp_FillGradientRect, 0x88),
	SLOT_OFF(TR_display_AlphaBlendRect, a_disp_AlphaBlendRect, 0x8C),
	SLOT_OFF(TR_display_DrawImage, a_disp_DrawImage, 0x90),
	SLOT_OFF(TR_display_DrawBitmap, a_disp_DrawBitmap, 0x94),
	SLOT(TR_display_DrawBitmapEx, a_display_DrawBitmapEx),
	SLOT_OFF(TR_display_DrawBitmapFrame, a_disp_DrawBitmapFrame, 0x9C),
	SLOT(TR_display_CreateBitmap, a_zm_display_CreateBitmap),
	SLOT(TR_display_LoadBitmap, a_zm_display_LoadBitmap),
	SLOT_OFF(TR_display_CreateImage, a_disp_CreateImage, 0xA8),
	SLOT(TR_display_BitBlt, a_display_BitBlt),
	SLOT_OFF(TR_display_Flatten, a_disp_Flatten, 0xB0),
	SLOT(TR_display_StretchBlt, a_display_StretchBlt),
	SLOT_OFF(TR_display_DrawAntialiasingLine, a_disp_DrawAntialiasingLine, 0xB8),
	SLOT_OFF(TR_display_DrawWLine, a_disp_DrawWLine, 0xBC),
	SLOT_OFF(TR_display_GetDMLayerHdlr, a_disp_GetDMLayerHdlr, 0xC0),
	SLOT_OFF(TR_display_RelevanceLayer, a_disp_RelevanceLayer, 0xC4),
	SLOT(TR_display_Refresh, a_zm_display_Refresh),
	SLOT_OFF(TR_display_DrawImageExt, a_disp_DrawImageExt, 0xCC),
	SLOT_OFF(TR_display_DrawSysWallPaper, a_disp_DrawSysWallPaper, 0xD0),
	SLOT_OFF(TR_display_DrawBorderText, a_disp_DrawBorderText, 0xD4),
	SLOT_OFF(TR_display_PushAndSetAlphaLayer, a_disp_PushAndSetAlphaLayer, 0xD8),
	SLOT_OFF(TR_display_PopAndRestoreAlphaLayer, a_disp_PopAndRestoreAlphaLayer, 0xDC),
	SLOT_OFF(TR_display_RotateScreen, a_disp_RotateScreen, 0xE0),

	/* ---- surf（精确项须先于区间）---- */
	SLOT(TR_surf_release, a_zm_surf_release),
	SLOT(TR_surf_getrect, a_zm_surf_getrect),
	RANGE(TR_surf_x04, TR_surf_x50, d_surf_nop),

	/* ---- image ---- */
	SLOT(TR_image_AddRef, a_zm_image_AddRef),
	SLOT(TR_image_Release, a_zm_image_Release),
	SLOT(TR_image_SetData, a_zm_image_SetData),
	SLOT(TR_image_GetFrameCount, a_zm_image_GetFrameCount),
	SLOT(TR_image_Width, a_zm_image_Width),
	SLOT(TR_image_Height, a_zm_image_Height),
	SLOT(TR_image_GetType, a_zm_image_GetType),
	SLOT(TR_image_Decode, a_image_Decode),
	RANGE(TR_image_x20, TR_image_x3C, d_image_stub),

	/* ---- bitmap ---- */
	SLOT(TR_bitmap_AddRef, a_zm_bitmap_AddRef),
	SLOT(TR_bitmap_Release, a_zm_bitmap_Release),
	SLOT(TR_bitmap_SetTransColor, a_zm_bitmap_SetTransColor),
	SLOT_OFF(TR_bitmap_sub_25F78, a_bmp_25F78, 0x0C),
	SLOT(TR_bitmap_GetInfo, a_zm_bitmap_GetInfo),
	SLOT_OFF(TR_bitmap_sub_25F84, a_bmp_25F84, 0x14),
	SLOT_OFF(TR_bitmap_sub_25FF8, a_bmp_25FF8, 0x18),

	/* ---- media ---- */
	SLOT(TR_media_AddRef, a_media_AddRef),
	SLOT(TR_media_Release, a_media_Release),
	SLOT_OFF(TR_media_x08, a_media_stub, 0x08),
	SLOT_OFF(TR_media_x0C, a_media_stub, 0x0C),
	SLOT(TR_media_play, a_media_play),
	SLOT(TR_media_stop, a_zm_media_stop),
	SLOT(TR_media_x18, a_zm_media_pause_music),
	SLOT(TR_media_x1C, a_zm_media_resume_music),
	SLOT_OFF(TR_media_x20, a_media_stub, 0x20),
	SLOT_OFF(TR_media_x24, a_media_stub, 0x24),
	SLOT_OFF(TR_media_x28, a_media_stub, 0x28),
	SLOT_OFF(TR_media_x2C, a_media_stub, 0x2C),
	SLOT_OFF(TR_media_x30, a_media_stub, 0x30),
	SLOT_OFF(TR_media_x34, a_media_stub, 0x34),
	SLOT(TR_media_x38, a_media_x38),
	SLOT_OFF(TR_media_x3C, a_media_stub, 0x3C),
	SLOT(TR_media_x40, a_media_x40),
	SLOT_OFF(TR_media_x44, a_media_stub, 0x44),
	SLOT_OFF(TR_media_x48, a_media_stub, 0x48),
	SLOT_OFF(TR_media_x4C, a_media_stub, 0x4C),
	SLOT_OFF(TR_media_x50, a_media_stub, 0x50),
	SLOT(TR_media_x54, a_media_x54),
	SLOT_OFF(TR_media_x58, a_media_stub, 0x58),

	/* ---- setting ---- */
	SLOT(TR_setting_AddRef, a_setting_AddRef),
	SLOT(TR_setting_Release, a_zm_svc_release),
	SLOT_OFF(TR_setting_x08, a_setting_stub, 0x08),
	SLOT_OFF(TR_setting_x0C, a_setting_stub, 0x0C),
	SLOT_OFF(TR_setting_x10, a_setting_stub, 0x10),
	SLOT_OFF(TR_setting_x14, a_setting_stub, 0x14),
	SLOT(TR_setting_x18, a_setting_x18),
	SLOT(TR_setting_x1C, a_setting_x1C),
	SLOT(TR_setting_x20, a_setting_x20),
	SLOT(TR_setting_x24, a_setting_x24),
	SLOT_OFF(TR_setting_x28, a_setting_stub, 0x28),
	SLOT_OFF(TR_setting_x2C, a_setting_stub, 0x2C),
	SLOT_OFF(TR_setting_x30, a_setting_stub, 0x30),
	SLOT_OFF(TR_setting_x34, a_setting_stub, 0x34),

	/* ---- netmgr ---- */
	SLOT(TR_netmgr_release, a_zm_svc_release),
	SLOT(TR_netmgr_x1C, a_zm_netmgr_x1C),

	/* ---- tapi（精确项须先于区间）---- */
	SLOT(TR_tapi_x2C, a_zm_tapi_x2C),
	SLOT(TR_tapi_x40, a_zm_tapi_x40),
	SLOT(TR_tapi_release, a_zm_svc_release),
	RANGE(TR_tapi_x00, TR_tapi_x50, d_tapi_stub),

	/* ---- zip 区间 ---- */
	RANGE(TR_zip_x00, TR_zip_x10, d_zip_stub),

	/* ---- dll ---- */
	SLOT(TR_dll_release, a_zm_svc_release),
	SLOT(TR_dll_init, a_zm_dll_init),
	SLOT(TR_dll_config, a_dll_config),
	SLOT(TR_dll_entry, a_dll_entry),

	/* ---- cbk ---- */
	SLOT(TR_cbk_default, a_zm_root_cbk_default),
};

/* 线性查表（首条命中即返回，故精确项须排在各自区间之前） */
static const trap_entry *trap_lookup(uint32_t addr) {
	for (size_t i = 0; i < sizeof(k_trap_table) / sizeof(k_trap_table[0]); i++) {
		if (addr >= k_trap_table[i].lo && addr <= k_trap_table[i].hi)
			return &k_trap_table[i];
	}
	return NULL;
}

/* 兜底：原 switch 的 default 分支（IBitmap 原生虚表 + 未接线槽告警），behavior 不变 */
static uint32_t trap_default(trap_ctx *c) {
	uint32_t trap_address = c->trap;
	if (trap_address >= TRAMP_BASE + 0x1700 && trap_address < TRAMP_BASE + 0x1720) {
		uint32_t off = trap_address - (TRAMP_BASE + 0x1700);
		if (off == 0x10) { /* GetInfo(this, out)：RE memcpy(out, obj+8, 0x20) */
			if (!c->r0 || !c->r1)
				return (uint32_t)-4;
			uint8_t info[0x20];
			if (uc_mem_read(c->uc, c->r0 + 8, info, sizeof(info)) != UC_ERR_OK)
				return (uint32_t)-4;
			uc_mem_write(c->uc, c->r1, info, sizeof(info));
			return 0;
		}
		if (off == 0x08) { /* SetTransColor(this, color) → 对象 +0x20 */
			if (c->r0)
				uc_mem_write(c->uc, c->r0 + 20, &c->r1, sizeof(c->r1));
			return 0;
		}
		return 0; /* AddRef / Release / 其余未知槽：真机亦返回 0 */
	}
	/* CBK 陷阱窗口里**没登记**的地址（applet 按"相对表"算出的偏移落在窗口中间）：
	 * 安静返回 0 即可 —— 这里不是"非法调用"，而是这一族的探路 ✓（见 emu.c 里
	 * "陷阱窗口同时当相对偏移表"的说明）。 */
	if (trap_address >= CBK_TRAP_BASE && trap_address < CBK_TRAP_BASE + CBK_TRAP_SIZE) {
		log_debug("CBK 陷阱窗口未登记槽 +0x%X → 返回 0 (r0=0x%X r1=0x%X lr=0x%X)",
				  trap_address - CBK_TRAP_BASE,
				  c->r0,
				  c->r1,
				  c->lr);
		return 0;
	}
	if (trap_address >= TRAMP_BASE && trap_address < TRAMP_BASE + TRAMP_SIZE) {
		uint32_t slot = trap_address - TRAMP_BASE;
		if (getenv("ZM_SHOW_TRACE") && (slot == 0x70 || slot == 0x12C)) {
			log_info("[SHIM细] slot=0x%X r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X",
					 slot,
					 c->r0,
					 c->r1,
					 c->r2,
					 c->r3,
					 c->lr);
			for (int i = 0; i < 10; i++)
				log_info(
					"[SHIM细]   sp+%02d = %08X", i * 4, uc_read32(c->uc, c->sp + (uint32_t)i * 4));
			if (slot == 0x70) {
				uint32_t dp = uc_read32(c->uc, c->r0 + 0);
				uint32_t ln = uc_read32(c->uc, c->r0 + 4);
				uint8_t bb[32];
				if (dp)
					uc_mem_read(c->uc, dp, bb, sizeof(bb));
				log_info(
					"[SHIM细] CString@%08X dp=%08X len=%08X "
					"data=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
					c->r0,
					dp,
					ln,
					bb[0],
					bb[1],
					bb[2],
					bb[3],
					bb[4],
					bb[5],
					bb[6],
					bb[7],
					bb[8],
					bb[9],
					bb[10],
					bb[11],
					bb[12],
					bb[13],
					bb[14],
					bb[15]);
			}
		}
		log_error("非法的外部调用: 0x%08X (SHIM槽+0x%X) r0=0x%X r1=0x%X "
				  "r2=0x%X r3=0x%X sp[0]=0x%X sp[4]=0x%X lr=0x%X",
				  trap_address,
				  slot,
				  c->r0,
				  c->r1,
				  c->r2,
				  c->r3,
				  uc_read32(c->uc, c->sp),
				  uc_read32(c->uc, c->sp + 4),
				  c->lr);
	} else {
		log_error("非法的外部调用: 0x%08" PRIx32, trap_address);
	}
	return 0;
}

void handle_trap(uc_engine *uc, uint32_t trap_address) {
	uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, sp = 0, lr = 0;
	/* 逐个检查寄存器读取结果，避免失败时使用未初始化值污染分发逻辑 */
	if (uc_reg_read(uc, UC_ARM_REG_R0, &r0) != UC_ERR_OK ||
		uc_reg_read(uc, UC_ARM_REG_R1, &r1) != UC_ERR_OK ||
		uc_reg_read(uc, UC_ARM_REG_R2, &r2) != UC_ERR_OK ||
		uc_reg_read(uc, UC_ARM_REG_R3, &r3) != UC_ERR_OK ||
		uc_reg_read(uc, UC_ARM_REG_SP, &sp) != UC_ERR_OK ||
		uc_reg_read(uc, UC_ARM_REG_LR, &lr) != UC_ERR_OK) {
		log_error("Failed to read core registers at trap 0x%08" PRIx32, trap_address);
		return;
	}

	trap_debug_before(uc, trap_address, r0, r1, r2, r3, lr);

	const trap_entry *e = trap_lookup(trap_address);
	trap_ctx c = {.uc = uc,
				  .trap = trap_address,
				  .r0 = r0,
				  .r1 = r1,
				  .r2 = r2,
				  .r3 = r3,
				  .lr = lr,
				  .sp = sp,
				  .off = e ? e->off : 0u};

	int ring = trap_ring_record(trap_address, r0, r1, r2, r3, lr);
	uint32_t ret = e ? e->fn(&c) : trap_default(&c);
	trap_debug_after(ring, ret);
	trap_debug_probe(trap_address, r0, r1, r2, r3, lr);

	/* 带上槽号：排查"某个返回值被 applet 存进对象、之后当指针用"的场景时，
	 * 只打 r0 的值分不清是哪个槽返回的。这里只改调试输出，不改任何行为。
	 *
	 * TK_MANUAL_PC 的 handler 已自行写 PC / 停 emu，这里不能再写 R0/PC。 */
	if (!e || e->kind == TK_CALL) {
		if (trap_address >= TRAMP_BASE && trap_address < TRAMP_BASE + TRAMP_SIZE)
			log_debug("applet 调用外部[槽+0x%X] 返回 r0=0x%X (入参 r0=0x%X r1=0x%X "
					  "r2=0x%X r3=0x%X lr=0x%X)",
					  trap_address - TRAMP_BASE,
					  ret,
					  r0,
					  r1,
					  r2,
					  r3,
					  lr);
		else
			log_debug("applet 调用外部(0x%X) 返回 r0=0x%X (入参 r0=0x%X r1=0x%X "
					  "r2=0x%X r3=0x%X lr=0x%X)",
					  trap_address,
					  ret,
					  r0,
					  r1,
					  r2,
					  r3,
					  lr);

		uc_reg_write(uc, UC_ARM_REG_R0, &ret);
		uc_reg_write(uc, UC_ARM_REG_PC, &lr);
	}
}

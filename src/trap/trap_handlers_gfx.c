/**
 * @file trap_handlers_gfx.c
 * @brief IDisplay / ISurface / IImage / IBitmap 的槽位 handler
 *
 * display 的绝大多数槽是"同一个实函数 + 不同的槽偏移"，以前靠 AD_OFF 宏
 * 把偏移烧进函数名（a_off_disp_SetClipRect）；现在偏移是**分派表的一列**
 * （见 trap_dispatch.c），handler 里只读 c->off。
 */

#include "../emu.h"
#include "../log/log.h"
#include "../tool/uc_helper.h" /* uc_read32 */
#include "../traps/emu_bitmap_traps.h"
#include "../zmaee/gfx/zm_display.h" /* ZMAEE IDisplay / IBitmap + SDL 渲染后端 */
#include "../zmaee/gfx/zm_image.h"	 /* ZMAEE IImage / ISurface */
#include "trap_internal.h"

/* ==========================================================================
 * IDisplay：固定参数槽
 * ========================================================================== */

uint32_t a_zm_display_AddRef(trap_ctx *c) {
	return zm_display_AddRef(c->uc, c->r0);
}

uint32_t a_zm_display_Release(trap_ctx *c) {
	return zm_display_Release(c->uc, c->r0);
}

uint32_t a_zm_display_GetMaxLayerCount(trap_ctx *c) {
	return zm_display_GetMaxLayerCount(c->uc, c->r0);
}

uint32_t a_zm_display_FreeAllLayer(trap_ctx *c) {
	return zm_display_FreeAllLayer(c->uc, c->r0);
}

uint32_t a_zm_display_GetActiveLayer(trap_ctx *c) {
	return zm_display_GetActiveLayer(c->uc, c->r0);
}

uint32_t a_zm_display_UnlockScreen(trap_ctx *c) {
	return zm_display_UnlockScreen(c->uc, c->r0);
}

uint32_t a_zm_display_GetFontHeight(trap_ctx *c) {
	return zm_display_GetFontHeight(c->uc);
}

uint32_t a_zm_display_FreeLayer(trap_ctx *c) {
	return zm_display_FreeLayer(c->uc, c->r0, c->r1);
}

uint32_t a_zm_display_SetActiveLayer(trap_ctx *c) {
	return zm_display_SetActiveLayer(c->uc, c->r0, c->r1);
}

uint32_t a_zm_display_SelectFont(trap_ctx *c) {
	return zm_display_SelectFont(c->uc, c->r0, c->r1);
}

uint32_t a_zm_display_GetFontWidth(trap_ctx *c) {
	return zm_display_GetFontWidth(c->uc, c->r0, c->r1);
}

uint32_t a_zm_display_LoadBitmap(trap_ctx *c) {
	return zm_display_LoadBitmap(c->uc, c->r0, c->r1);
}

uint32_t a_zm_display_SetTransColor(trap_ctx *c) {
	return zm_display_SetTransColor(c->uc, c->r0, c->r1, c->r2);
}

uint32_t a_zm_display_Refresh(trap_ctx *c) {
	return zm_display_Refresh(c->uc);
}

uint32_t a_zm_display_UpdateEx(trap_ctx *c) {
	return zm_display_UpdateEx(c->uc, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_zm_display_CreateBitmap(trap_ctx *c) {
	return zm_display_CreateBitmap(c->uc, c->r0, c->r1, c->r2, c->r3);
}

/* ---- 需要第 5 个及以后的参数（从栈上读）---- */

uint32_t a_display_Update(trap_ctx *c) {
	return zm_display_Update(c->uc, c->r0, c->r1, c->r2, c->r3, getArg(c->uc, 4));
}

uint32_t a_display_MeasureString(trap_ctx *c) {
	return zm_display_MeasureString(c->uc, c->r0, c->r1, c->r2, c->r3, c->sp);
}

uint32_t a_display_DrawText(trap_ctx *c) {
	return zm_display_DrawText(c->uc, c->r1, c->r2, c->r3, c->sp);
}

uint32_t a_display_DrawRect(trap_ctx *c) {
	return zm_display_DrawRect(c->uc, c->r1, c->r2, c->r3, c->sp);
}

uint32_t a_display_FillRect(trap_ctx *c) {
	return zm_display_FillRect(c->uc, c->r1, c->r2, c->r3, c->sp);
}

uint32_t a_display_DrawBitmapEx(trap_ctx *c) {
	if (g_disasm) {
		static int watch_done = 0;
		if (!watch_done && c->r3 >= 0x820000) {
			uint32_t b0 = uc_read32(c->uc, c->r3), b1 = uc_read32(c->uc, c->r3 + 4);
			uint32_t pc0 = 0;
			uc_reg_read(c->uc, UC_ARM_REG_R4, &pc0);
			log_debug("WATCH bmp=0x%X first=[%08X %08X] R4=0x%X", c->r3, b0, b1, pc0);
			watch_done = 1;
		}
		uint32_t b0 = uc_read32(c->uc, c->r3), b1 = uc_read32(c->uc, c->r3 + 4);
		uint32_t b2 = uc_read32(c->uc, c->r3 + 8);
		log_debug("DrawBitmapEx lr=0x%X x=%u y=%u bmp=0x%X(=[%08X %08X %08X]) "
				  "rect=0x%X mode=%u",
				  c->lr,
				  c->r1,
				  c->r2,
				  c->r3,
				  b0,
				  b1,
				  b2,
				  getArg(c->uc, 4),
				  getArg(c->uc, 5));
	}
	return zm_display_DrawBitmapEx(c->uc, 0x98U, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_display_BitBlt(trap_ctx *c) {
	if (g_disasm) {
		uint32_t s0 = uc_read32(c->uc, c->r3), s1 = uc_read32(c->uc, c->r3 + 4);
		uint32_t s2 = uc_read32(c->uc, c->r3 + 8), s3 = uc_read32(c->uc, c->r3 + 12);
		log_debug("BitBlt lr=0x%X dx=%u dy=%u surf=0x%X(=[%08X %08X %08X %08X]) "
				  "rect=0x%X mode=%u flags=%u",
				  c->lr,
				  c->r1,
				  c->r2,
				  c->r3,
				  s0,
				  s1,
				  s2,
				  s3,
				  getArg(c->uc, 4),
				  getArg(c->uc, 5),
				  getArg(c->uc, 6));
	}
	return zm_display_BitBlt(c->uc, 0xACU, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_display_StretchBlt(trap_ctx *c) {
	static uint32_t sn = 0;
	if (sn++ < 6)
		log_info("[StretchBlt] lr=0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X",
				 c->lr,
				 c->r0,
				 c->r1,
				 c->r2,
				 c->r3);
	return zm_display_StretchBlt(c->uc, 0xB4U, c->r0, c->r1, c->r2, c->r3);
}

/* ==========================================================================
 * IDisplay：带槽偏移的槽（偏移在表项里，见 trap_dispatch.c）
 * ========================================================================== */

uint32_t a_disp_CreateLayer(trap_ctx *c) {
	return zm_display_CreateLayer(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_CreateLayerExt(trap_ctx *c) {
	return zm_display_CreateLayerExt(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_GetLayerInfo(trap_ctx *c) {
	return zm_display_GetLayerInfo(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_SetLayerPosition(trap_ctx *c) {
	return zm_display_SetLayerPosition(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_LockScreen(trap_ctx *c) {
	return zm_display_stub(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_RegisterCustomFont(trap_ctx *c) {
	return zm_display_RegisterCustomFont(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_SetOpacity(trap_ctx *c) {
	return zm_display_SetOpacity(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_SetClipRect(trap_ctx *c) {
	return zm_display_SetClipRect(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_GetClipRect(trap_ctx *c) {
	return zm_display_GetClipRect(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_SetPixel(trap_ctx *c) {
	return zm_display_SetPixel(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawLine(trap_ctx *c) {
	return zm_display_DrawLine(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawRoundRect(trap_ctx *c) {
	return zm_display_DrawRoundRect(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawCircle(trap_ctx *c) {
	return zm_display_DrawCircle(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_FillCircle(trap_ctx *c) {
	return zm_display_FillCircle(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawArc(trap_ctx *c) {
	return zm_display_DrawArc(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_FillArc(trap_ctx *c) {
	return zm_display_FillArc(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_FillGradientRect(trap_ctx *c) {
	return zm_display_FillGradientRect(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_AlphaBlendRect(trap_ctx *c) {
	return zm_display_AlphaBlendRect(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawImage(trap_ctx *c) {
	return zm_display_DrawImage(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawBitmap(trap_ctx *c) {
	return zm_display_DrawBitmap(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawBitmapFrame(trap_ctx *c) {
	return zm_display_DrawBitmapFrame(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_Flatten(trap_ctx *c) {
	return zm_display_Flatten(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_CreateImage(trap_ctx *c) {
	return zm_display_CreateImage(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawAntialiasingLine(trap_ctx *c) {
	return zm_display_DrawAntialiasingLine(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawWLine(trap_ctx *c) {
	return zm_display_DrawWLine(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_GetDMLayerHdlr(trap_ctx *c) {
	return zm_display_GetDMLayerHdlr(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_RelevanceLayer(trap_ctx *c) {
	return zm_display_RelevanceLayer(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawImageExt(trap_ctx *c) {
	return zm_display_DrawImageExt(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawSysWallPaper(trap_ctx *c) {
	return zm_display_DrawSysWallPaper(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_DrawBorderText(trap_ctx *c) {
	return zm_display_DrawBorderText(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_PushAndSetAlphaLayer(trap_ctx *c) {
	return zm_display_PushAndSetAlphaLayer(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_PopAndRestoreAlphaLayer(trap_ctx *c) {
	return zm_display_PopAndRestoreAlphaLayer(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_disp_RotateScreen(trap_ctx *c) {
	return zm_display_RotateScreen(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}

/* ==========================================================================
 * ISurface
 * ========================================================================== */

uint32_t a_zm_surf_release(trap_ctx *c) {
	return zm_surf_release(c->uc, c->r0);
}

uint32_t a_zm_surf_getrect(trap_ctx *c) {
	return zm_surf_getrect(c->uc, c->r0, c->r1);
}

/* 区间：offset 由 trap 地址自动算 */
uint32_t d_surf_nop(trap_ctx *c) {
	return zm_surf_nop(c->uc, c->trap - TRAMP_BASE);
}

/* ==========================================================================
 * IImage
 * ========================================================================== */

uint32_t a_zm_image_AddRef(trap_ctx *c) {
	return zm_image_AddRef(c->uc, c->r0);
}

uint32_t a_zm_image_Release(trap_ctx *c) {
	return zm_image_Release(c->uc, c->r0);
}

uint32_t a_zm_image_GetFrameCount(trap_ctx *c) {
	return zm_image_GetFrameCount(c->uc, c->r0);
}

uint32_t a_zm_image_Width(trap_ctx *c) {
	return zm_image_Width(c->uc, c->r0);
}

uint32_t a_zm_image_Height(trap_ctx *c) {
	return zm_image_Height(c->uc, c->r0);
}

uint32_t a_zm_image_GetType(trap_ctx *c) {
	return zm_image_GetType(c->uc, c->r0);
}

uint32_t a_zm_image_SetData(trap_ctx *c) {
	return zm_image_SetData(c->uc, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_image_Decode(trap_ctx *c) {
	return zm_image_DecodeToBitmap(c->uc, c->r0, c->r1, c->r2, c->r3, c->sp);
}

/* 区间：offset 由 trap 地址自动算 */
uint32_t d_image_stub(trap_ctx *c) {
	return zm_image_stub(c->uc, c->trap - TRAMP_BASE, c->r0, c->r1, c->r2, c->r3);
}

/* ==========================================================================
 * IBitmap
 * ========================================================================== */

uint32_t a_zm_bitmap_AddRef(trap_ctx *c) {
	return zm_bitmap_AddRef(c->uc, c->r0);
}

uint32_t a_zm_bitmap_Release(trap_ctx *c) {
	return zm_bitmap_Release(c->uc, c->r0);
}

uint32_t a_zm_bitmap_SetTransColor(trap_ctx *c) {
	return zm_bitmap_SetTransColor(c->uc, c->r0, c->r1);
}

uint32_t a_zm_bitmap_GetInfo(trap_ctx *c) {
	return zm_bitmap_GetInfo(c->uc, c->r0, c->r1);
}

/* 带槽偏移的三个槽（偏移在表项里） */
uint32_t a_bmp_25F78(trap_ctx *c) {
	return zm_bitmap_sub_25F78(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_bmp_25F84(trap_ctx *c) {
	return zm_bitmap_sub_25F84(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}
uint32_t a_bmp_25FF8(trap_ctx *c) {
	return zm_bitmap_sub_25FF8(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}

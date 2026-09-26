#ifndef EMU_BITMAP_TRAPS_H
#define EMU_BITMAP_TRAPS_H

#include "../emu_mem_layout.h"

/* -------------------- ZM_BITMAP_VT 枚举 -------------------- */
/* ZMAEE IBitmap 原生虚表槽位（基址 BITMAP_VT_ADDR，共 7 槽，止于 +0x18） */
enum ZM_BITMAP_VT : uint32_t {
	ZM_Bitmap_AddRef = 0x00U,
	ZM_Bitmap_Release = 0x04U,
	ZM_Bitmap_SetTransColor = 0x08U,
	ZM_Bitmap_sub_25F78 = 0x0CU,
	ZM_Bitmap_GetInfo = 0x10U,
	ZM_Bitmap_sub_25F84 = 0x14U,
	ZM_Bitmap_sub_25FF8 = 0x18U
};

/* ---- ZMAEE IBitmap 原生虚表（g_aee_bitmap_vtbl @ .data:0x63DF4）----
 * 偏移严格按逆向贴出的表；与 trap.c 中 0x1700~0x171C 的 dispatch 范围一致
 * （共 7 槽：0x1700~0x1718，表尾 0x171C）。
 *   +0x08 SetTransColor(this, color) → 对象 +0x14（实测）
 *   +0x10 GetInfo(this, out)          → memcpy(out, obj+8, 0x20)（实测）
 *   +0x0C/0x14/0x18 是 sub_25F78/sub_25F84/sub_25FF8（未知槽，当前转派到
 *        zm_bitmap_sub_25F78/84/FF8 的 stub 实现，返回 0）。
 * 注：若逆向补充第 8 个槽，必须同步扩展 trap.c 的 range 与这里的枚举。 */
enum ZM_BITMAP_TRAPS : uint32_t {
	TR_bitmap_AddRef = TRAP(BITMAP_VT_ADDR + ZM_Bitmap_AddRef),
	TR_bitmap_Release = TRAP(BITMAP_VT_ADDR + ZM_Bitmap_Release),
	TR_bitmap_SetTransColor = TRAP(BITMAP_VT_ADDR + ZM_Bitmap_SetTransColor),
	TR_bitmap_sub_25F78 = TRAP(BITMAP_VT_ADDR + ZM_Bitmap_sub_25F78),
	TR_bitmap_GetInfo = TRAP(BITMAP_VT_ADDR + ZM_Bitmap_GetInfo),
	TR_bitmap_sub_25F84 = TRAP(BITMAP_VT_ADDR + ZM_Bitmap_sub_25F84),
	TR_bitmap_sub_25FF8 = TRAP(BITMAP_VT_ADDR + ZM_Bitmap_sub_25FF8)
};

#endif /* EMU_BITMAP_TRAPS_H */
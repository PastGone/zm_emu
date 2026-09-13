#ifndef EMU_BITMAP_TRAPS_H
#define EMU_BITMAP_TRAPS_H

#include "emu_mem_layout.h"

/* -------------------- BITMAP_VT_ADDR 枚举 -------------------- */
/* ZMAEE IBitmap 原生虚表槽位（基址 BITMAP_VT_ADDR） */
enum ZM_BITMAP_VT {
  ZM_Bitmap_AddRef = 0x00U,
  ZM_Bitmap_Release = 0x04U,
  ZM_Bitmap_SetTransColor = 0x08U,
  ZM_Bitmap_sub_25F78 = 0x0CU,
  ZM_Bitmap_GetInfo = 0x10U,
  ZM_Bitmap_sub_25F84 = 0x14U,
  ZM_Bitmap_sub_25FF8 = 0x18U
};

/* ---- ZMAEE IBitmap 原生虚表（g_aee_bitmap_vtbl @ .data:0x63DF4）----
 * +0x0C/0x14/0x18 是 sub_25F78/sub_25F84/sub_25FF8（未知），接 stub。 */
#define TR_bitmap_AddRef TRAP(BITMAP_VT_ADDR + ZM_Bitmap_AddRef)
#define TR_bitmap_Release TRAP(BITMAP_VT_ADDR + ZM_Bitmap_Release)
#define TR_bitmap_SetTransColor TRAP(BITMAP_VT_ADDR + ZM_Bitmap_SetTransColor)
#define TR_bitmap_sub_25F78 TRAP(BITMAP_VT_ADDR + ZM_Bitmap_sub_25F78)
#define TR_bitmap_GetInfo TRAP(BITMAP_VT_ADDR + ZM_Bitmap_GetInfo)
#define TR_bitmap_sub_25F84 TRAP(BITMAP_VT_ADDR + ZM_Bitmap_sub_25F84)
#define TR_bitmap_sub_25FF8 TRAP(BITMAP_VT_ADDR + ZM_Bitmap_sub_25FF8)

#endif /* EMU_BITMAP_TRAPS_H */
#ifndef EMU_DISPLAY_TRAPS_H
#define EMU_DISPLAY_TRAPS_H

#include "emu_mem_layout.h"

/* -------------------- DISPLAY_VT_ADDR 枚举 -------------------- */
/* ZMAEE IDisplay 原生虚表槽位（基址 DISPLAY_VT_ADDR，共 58 槽，止于 +0xE4）。
 */
enum ZM_DISPLAY_VT {
  ZM_Display_AddRef = 0x00U,
  ZM_Display_Release = 0x04U,
  ZM_Display_GetMaxLayerCount = 0x08U,
  ZM_Display_CreateLayer = 0x0CU,
  ZM_Display_CreateLayerExt = 0x10U,
  ZM_Display_FreeLayer = 0x14U,
  ZM_Display_FreeAllLayer = 0x18U,
  ZM_Display_GetLayerInfo = 0x1CU,
  ZM_Display_SetActiveLayer = 0x20U,
  ZM_Display_SetLayerPosition = 0x24U,
  ZM_Display_Update = 0x28U,
  ZM_Display_UpdateEx = 0x2CU,
  ZM_Display_GetActiveLayer = 0x30U,
  ZM_Display_LockScreen = 0x34U,
  ZM_Display_UnlockScreen = 0x38U,
  ZM_Display_RegisterCustomFont = 0x3CU,
  ZM_Display_SelectFont = 0x40U,
  ZM_Display_GetFontWidth = 0x44U,
  ZM_Display_GetFontHeight = 0x48U,
  ZM_Display_MeasureString = 0x4CU,
  ZM_Display_DrawText = 0x50U,
  ZM_Display_SetTransColor = 0x54U,
  ZM_Display_SetOpacity = 0x58U,
  ZM_Display_SetClipRect = 0x5CU,
  ZM_Display_GetClipRect = 0x60U,
  ZM_Display_SetPixel = 0x64U,
  ZM_Display_DrawLine = 0x68U,
  ZM_Display_DrawRect = 0x6CU,
  ZM_Display_FillRect = 0x70U,
  ZM_Display_DrawRoundRect = 0x74U,
  ZM_Display_DrawCircle = 0x78U,
  ZM_Display_FillCircle = 0x7CU,
  ZM_Display_DrawArc = 0x80U,
  ZM_Display_FillArc = 0x84U,
  ZM_Display_FillGradientRect = 0x88U,
  ZM_Display_AlphaBlendRect = 0x8CU,
  ZM_Display_DrawImage = 0x90U,
  ZM_Display_DrawBitmap = 0x94U,
  ZM_Display_DrawBitmapEx = 0x98U,
  ZM_Display_DrawBitmapFrame = 0x9CU,
  ZM_Display_CreateBitmap = 0xA0U,
  ZM_Display_LoadBitmap = 0xA4U,
  ZM_Display_CreateImage = 0xA8U,
  ZM_Display_BitBlt = 0xACU,
  ZM_Display_Flatten = 0xB0U,
  ZM_Display_StretchBlt = 0xB4U,
  ZM_Display_DrawAntialiasingLine = 0xB8U,
  ZM_Display_DrawWLine = 0xBCU,
  ZM_Display_GetDMLayerHdlr = 0xC0U,
  ZM_Display_RelevanceLayer = 0xC4U,
  ZM_Display_Refresh = 0xC8U,
  ZM_Display_DrawImageExt = 0xCCU,
  ZM_Display_DrawSysWallPaper = 0xD0U,
  ZM_Display_DrawBorderText = 0xD4U,
  ZM_Display_PushAndSetAlphaLayer = 0xD8U,
  ZM_Display_PopAndRestoreAlphaLayer = 0xDCU,
  ZM_Display_RotateScreen = 0xE0U
};

/* ---- ZMAEE IDisplay 原生虚表（g_aee_display_vtbl @ .data:0x63E10）----
 * 偏移严格按逆向贴出的表。带「实测」的槽为旧 GFX 路径验证过的行为
 * （gfx 与 display 本是同一张表），其余为按槽序推测命名，语义待 RE 校准。
 * DISPLAY_VT_ADDR 58 槽止于 +0xE4，更远的偏移（如旧 GFX 路径见过的 +0x114）
 * 不在本表内，属其它对象/越界调用。 */
#define TR_display_AddRef TRAP(DISPLAY_VT_ADDR + ZM_Display_AddRef)
#define TR_display_Release TRAP(DISPLAY_VT_ADDR + ZM_Display_Release)
#define TR_display_GetMaxLayerCount \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_GetMaxLayerCount)
#define TR_display_CreateLayer TRAP(DISPLAY_VT_ADDR + ZM_Display_CreateLayer)
#define TR_display_CreateLayerExt \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_CreateLayerExt)
#define TR_display_FreeLayer TRAP(DISPLAY_VT_ADDR + ZM_Display_FreeLayer)

#define TR_display_FreeAllLayer \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_FreeAllLayer) /* 真机虚表 +0x18 = FreeAllLayer（实测被调，功能未知 stub） */

#define TR_display_GetLayerInfo TRAP(DISPLAY_VT_ADDR + ZM_Display_GetLayerInfo)

#define TR_display_SetActiveLayer \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_SetActiveLayer) /* 真机虚表 +0x20 = SetActiveLayer(display, idx) */

#define TR_display_SetLayerPosition \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_SetLayerPosition)
#define TR_display_Update TRAP(DISPLAY_VT_ADDR + ZM_Display_Update)

#define TR_display_UpdateEx \
  TRAP(DISPLAY_VT_ADDR + \
       ZM_Display_UpdateEx) /* 真机虚表 +0x2C = UpdateEx(display, rect, count, layerList) */

#define TR_display_GetActiveLayer \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_GetActiveLayer)

#define TR_display_LockScreen \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_LockScreen) /* 真机虚表 +0x34 = LockScreen（实测被调，功能未知 stub） */

#define TR_display_UnlockScreen TRAP(DISPLAY_VT_ADDR + ZM_Display_UnlockScreen)
#define TR_display_RegisterCustomFont \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_RegisterCustomFont)

#define TR_display_SelectFont \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_SelectFont) /* 真机虚表 +0x40 = SelectFont（实现见 zm_display.c） */

#define TR_display_GetFontWidth TRAP(DISPLAY_VT_ADDR + ZM_Display_GetFontWidth) /* 真机虚表 +0x44 = GetFontWidth（实现见 zm_display.c） */

#define TR_display_GetFontHeight \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_GetFontHeight) /* 真机虚表 +0x48 = GetFontHeight（实现见 zm_display.c） */

#define TR_display_MeasureString \
  TRAP(DISPLAY_VT_ADDR + \
       ZM_Display_MeasureString) /* 真机虚表 +0x4C = MeasureString(disp, str_ptr, len, width_out, sp[metrics]) */

#define TR_display_DrawText \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawText) /* 实测吻合：drawText */

#define TR_display_SetTransColor \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_SetTransColor)
#define TR_display_SetOpacity TRAP(DISPLAY_VT_ADDR + ZM_Display_SetOpacity)
#define TR_display_SetClipRect TRAP(DISPLAY_VT_ADDR + ZM_Display_SetClipRect)
#define TR_display_GetClipRect TRAP(DISPLAY_VT_ADDR + ZM_Display_GetClipRect)
#define TR_display_SetPixel TRAP(DISPLAY_VT_ADDR + ZM_Display_SetPixel)
#define TR_display_DrawLine TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawLine)

#define TR_display_DrawRect \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawRect) /* 实测吻合：drawRect */

#define TR_display_FillRect \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_FillRect) /* 实测吻合：fillRect */

#define TR_display_DrawRoundRect \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawRoundRect)
#define TR_display_DrawCircle TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawCircle)
#define TR_display_FillCircle TRAP(DISPLAY_VT_ADDR + ZM_Display_FillCircle)
#define TR_display_DrawArc TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawArc)
#define TR_display_FillArc TRAP(DISPLAY_VT_ADDR + ZM_Display_FillArc)
#define TR_display_FillGradientRect \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_FillGradientRect)
#define TR_display_AlphaBlendRect \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_AlphaBlendRect)
#define TR_display_DrawImage TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawImage)
#define TR_display_DrawBitmap TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawBitmap)
#define TR_display_DrawBitmapEx TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawBitmapEx)
#define TR_display_DrawBitmapFrame \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawBitmapFrame)

#define TR_display_CreateBitmap \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_CreateBitmap) /* 返回 BITMAP 单例 */

#define TR_display_LoadBitmap \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_LoadBitmap) /* 返回 BITMAP 单例 */

#define TR_display_CreateImage TRAP(DISPLAY_VT_ADDR + ZM_Display_CreateImage)
#define TR_display_BitBlt TRAP(DISPLAY_VT_ADDR + ZM_Display_BitBlt)
#define TR_display_Flatten TRAP(DISPLAY_VT_ADDR + ZM_Display_Flatten)
#define TR_display_StretchBlt TRAP(DISPLAY_VT_ADDR + ZM_Display_StretchBlt)
#define TR_display_DrawAntialiasingLine \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawAntialiasingLine)
#define TR_display_DrawWLine TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawWLine)
#define TR_display_GetDMLayerHdlr \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_GetDMLayerHdlr)
#define TR_display_RelevanceLayer \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_RelevanceLayer)
#define TR_display_Refresh TRAP(DISPLAY_VT_ADDR + ZM_Display_Refresh)
#define TR_display_DrawImageExt TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawImageExt)
#define TR_display_DrawSysWallPaper \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawSysWallPaper)
#define TR_display_DrawBorderText \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_DrawBorderText)
#define TR_display_PushAndSetAlphaLayer \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_PushAndSetAlphaLayer)
#define TR_display_PopAndRestoreAlphaLayer \
  TRAP(DISPLAY_VT_ADDR + ZM_Display_PopAndRestoreAlphaLayer)
#define TR_display_RotateScreen TRAP(DISPLAY_VT_ADDR + ZM_Display_RotateScreen)

#endif /* EMU_DISPLAY_TRAPS_H */

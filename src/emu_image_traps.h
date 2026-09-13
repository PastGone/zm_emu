#ifndef EMU_IMAGE_TRAPS_H
#define EMU_IMAGE_TRAPS_H

#include "emu_mem_layout.h"

/* -------------------- IMAGE_VT_ADDR 枚举 -------------------- */
/* ZMAEE IImage 原生虚表槽位（基址 IMAGE_VT_ADDR） */
/* ZMAEE IImage 原生虚表（与 g_aee_image_vtbl 逐槽对齐：
 *   AddRef/Release/SetData/GetFrameCount/Width/Height/GetType/Decode…） */
enum ZM_IMAGE_VT {
  ZM_Image_AddRef = 0x00U,
  ZM_Image_Release = 0x04U,
  ZM_Image_SetData = 0x08U,
  ZM_Image_GetFrameCount = 0x0CU,
  ZM_Image_Width = 0x10U,
  ZM_Image_Height = 0x14U,
  ZM_Image_GetType = 0x18U,
  ZM_Image_Decode = 0x1CU,
  ZM_Image_x20 = 0x20U,
  ZM_Image_x24 = 0x24U,
  ZM_Image_x28 = 0x28U,
  ZM_Image_x2C = 0x2CU,
  ZM_Image_x30 = 0x30U,
  ZM_Image_x34 = 0x34U,
  ZM_Image_x38 = 0x38U,
  ZM_Image_x3C = 0x3CU
};

/* -------------------- SURF_VT_ADDR 枚举 -------------------- */
/* ZMAEE surface 门面虚表槽位（基址 SURF_VT_ADDR，共 21 槽 / 0x54 字节） */
enum ZM_SURF_VT {
  ZM_Surf_release = 0x00U,
  ZM_Surf_x04 = 0x04U,
  ZM_Surf_x08 = 0x08U,
  ZM_Surf_x0C = 0x0CU,
  ZM_Surf_getrect = 0x10U,
  ZM_Surf_x14 = 0x14U,
  ZM_Surf_x18 = 0x18U,
  ZM_Surf_x1C = 0x1CU,
  ZM_Surf_x20 = 0x20U,
  ZM_Surf_x24 = 0x24U,
  ZM_Surf_x28 = 0x28U,
  ZM_Surf_x2C = 0x2CU,
  ZM_Surf_x30 = 0x30U,
  ZM_Surf_x34 = 0x34U,
  ZM_Surf_x38 = 0x38U,
  ZM_Surf_x3C = 0x3CU,
  ZM_Surf_x40 = 0x40U,
  ZM_Surf_x44 = 0x44U,
  ZM_Surf_x48 = 0x48U,
  ZM_Surf_x4C = 0x4CU,
  ZM_Surf_x50 = 0x50U
};

/* ---- ZMAEE IImage 原生虚表（IDisplay::CreateImage 造出的解码器对象）----
 * 槽位按 g_aee_image_vtbl 实测定：
 *   +0x08 SetData(this, 0, name_ptr, len) —— 按文件名装入（0=成功）
 *   +0x10 Width / +0x14 Height           —— 取宽高（像素）
 *   +0x18 GetType                        —— 类型枚举（applet 未据此分支）
 *   +0x1C Decode(this, alloc, free, &bmp, 0) —— 解码出 IBitmap（0=成功）
 * 其余槽接 zm_image_stub，保证不落"非法的外部调用"。 */
#define TR_image_AddRef TRAP(IMAGE_VT_ADDR + ZM_Image_AddRef)
#define TR_image_Release TRAP(IMAGE_VT_ADDR + ZM_Image_Release)
#define TR_image_SetData TRAP(IMAGE_VT_ADDR + ZM_Image_SetData)
#define TR_image_GetFrameCount TRAP(IMAGE_VT_ADDR + ZM_Image_GetFrameCount)
#define TR_image_Width TRAP(IMAGE_VT_ADDR + ZM_Image_Width)
#define TR_image_Height TRAP(IMAGE_VT_ADDR + ZM_Image_Height)
#define TR_image_GetType TRAP(IMAGE_VT_ADDR + ZM_Image_GetType)
#define TR_image_Decode TRAP(IMAGE_VT_ADDR + ZM_Image_Decode)
#define TR_image_x20 TRAP(IMAGE_VT_ADDR + ZM_Image_x20)
#define TR_image_x24 TRAP(IMAGE_VT_ADDR + ZM_Image_x24)
#define TR_image_x28 TRAP(IMAGE_VT_ADDR + ZM_Image_x28)
#define TR_image_x2C TRAP(IMAGE_VT_ADDR + ZM_Image_x2C)
#define TR_image_x30 TRAP(IMAGE_VT_ADDR + ZM_Image_x30)
#define TR_image_x34 TRAP(IMAGE_VT_ADDR + ZM_Image_x34)
#define TR_image_x38 TRAP(IMAGE_VT_ADDR + ZM_Image_x38)
#define TR_image_x3C TRAP(IMAGE_VT_ADDR + ZM_Image_x3C)

/* ---- ZMAEE surface 门面虚表（IImage::Decode 的 out 对象）----
 * 对象字段（00000506 运行期确认）：
 *   +0 = vt   +4 = 原始 surface 指针   +8 = 宽   +0xC = 高
 * 关键槽（逆向 sub_388 type1 / sub_4A0）：
 *   +0x10 GetRect(this, out) → 写 int16 矩形 {l,t,r,b}（分派器据此绘制）
 * 其它槽按对象族的常规顺序给安全的空实现/固定值，避免落到"非法外部调用"。
 * 共 21 槽（0x54 字节）。 */
#define TR_surf_release TRAP(SURF_VT_ADDR + ZM_Surf_release) /* 析构 */
#define TR_surf_x04 TRAP(SURF_VT_ADDR + ZM_Surf_x04)
#define TR_surf_x08 TRAP(SURF_VT_ADDR + ZM_Surf_x08)
#define TR_surf_x0C TRAP(SURF_VT_ADDR + ZM_Surf_x0C)

#define TR_surf_getrect                                                        \
  TRAP(SURF_VT_ADDR + ZM_Surf_getrect) /* GetRect(this,out)：实测使用 */

#define TR_surf_x14 TRAP(SURF_VT_ADDR + ZM_Surf_x14)
#define TR_surf_x18 TRAP(SURF_VT_ADDR + ZM_Surf_x18)
#define TR_surf_x1C TRAP(SURF_VT_ADDR + ZM_Surf_x1C)
#define TR_surf_x20 TRAP(SURF_VT_ADDR + ZM_Surf_x20)
#define TR_surf_x24 TRAP(SURF_VT_ADDR + ZM_Surf_x24)
#define TR_surf_x28 TRAP(SURF_VT_ADDR + ZM_Surf_x28)
#define TR_surf_x2C TRAP(SURF_VT_ADDR + ZM_Surf_x2C)
#define TR_surf_x30 TRAP(SURF_VT_ADDR + ZM_Surf_x30)
#define TR_surf_x34 TRAP(SURF_VT_ADDR + ZM_Surf_x34)
#define TR_surf_x38 TRAP(SURF_VT_ADDR + ZM_Surf_x38)
#define TR_surf_x3C TRAP(SURF_VT_ADDR + ZM_Surf_x3C)
#define TR_surf_x40 TRAP(SURF_VT_ADDR + ZM_Surf_x40)
#define TR_surf_x44 TRAP(SURF_VT_ADDR + ZM_Surf_x44)
#define TR_surf_x48 TRAP(SURF_VT_ADDR + ZM_Surf_x48)
#define TR_surf_x4C TRAP(SURF_VT_ADDR + ZM_Surf_x4C)
#define TR_surf_x50 TRAP(SURF_VT_ADDR + ZM_Surf_x50)

#endif /* EMU_IMAGE_TRAPS_H */
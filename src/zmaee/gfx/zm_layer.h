#ifndef ZM_LAYER_H
#define ZM_LAYER_H

#include <stdint.h>
#include <unicorn/unicorn.h>

/*
 * ZMAEE IDisplay 的"层"子系统（RE：ZMAEE_IDisplay_CreateLayer /
 * GetLayerInfo / SetLayerPosition / FillRect / UpdateEx）。
 *
 * 布局（三处反编译互相印证）：
 *   层数组起点 = IDisplay + 36，每项 52(0x34) 字节
 *   IDisplay + 8  = 活动层索引           （SetActiveLayer 写这里）
 *   IDisplay + 20 = 每层一个字节的标志数组，索引 idx 处 = "该层缓冲自持"
 *   第 idx 层的项基址 = IDisplay + 52*idx
 *   项载荷 = 项基址 + 36，共 0x34 字节：
 *     +0x00 色深(1=RGB565, 2/3/4=32bit)   +0x04 x   +0x08 y
 *     +0x0C 宽(兼 pitch)                  +0x10 高
 *     +0x14 裁剪 x   +0x18 裁剪 y   +0x1C 宽副本   +0x20 高副本
 *     +0x24 像素缓冲指针                   +0x2C 透明色   +0x30 掩码
 *
 * 关键推论：
 *   - FillRect / DrawBitmap / DrawImage / UpdateEx 全都用
 *     `&v7[13 * v7[2] + 9]`（v7[2] = *(IDisplay+8)）定位层，所以 +8 必须正确，
 *     否则 applet 自带 GDI 算出的层地址是错的。
 *   - ZMCF→字节/像素：0→1、1→2、2/3/4→4（ZMCF2BytsPerPixel 的表）。
 *   - 色深 1 = RGB565：FillRect 用 ((c&0xF80000)>>8)|((c&0xFC00)>>5)|((c&0xFF)>>3)。
 */

/* 第 idx 层项载荷的绝对地址 */
uint32_t zm_layer_payload(uint32_t display, uint32_t idx);

/* ZMCF（层载荷 +0x00 / CreateLayer 的 a4 / 基础层深度）→ 每像素字节数。
 *
 * RE 已确认：.rodata:0x5B500 = {1, 2, 4, 4, 4}，由
 * ZMAEE_IDisplay_ZMCF2BytsPerPixel 索引；CreateLayerExt 里同款换算为
 * "a4==1 → 2*w*h，a4∈{2,3,4} → 4*w*h"。
 * 即：ZMCF 1 = RGB565(2 字节)，2/3/4 = 32bit(4 字节)。 */
extern const int ZM_CF_BPP[5];

/* 按 ZMCF 取每像素字节数（越界返回 0） */
static inline int zm_cf_bpp(uint32_t fmt) {
	return (fmt <= 4u) ? ZM_CF_BPP[fmt] : 0;
}

/* 按 RE 的 ZMAEE_IDisplay_New 语义建立"层 0"（基础层）。
 *
 * RE（000282EC ZMAEE_IDisplay_New）里层 0 是**内联构造**的，不走 CreateLayer
 * （CreateLayer 的 `(idx-1) > 0xE` 直接拒绝 idx=0），字段映射为：
 *   +0x00 = GetBaseLayerDepth()（屏幕色深对应的 ZMCF）
 *   +0x04 = 0            +0x08 = 0
 *   +0x0C = 屏宽         +0x10 = 屏高
 *   +0x14 = 0            +0x18 = 0          （裁剪原点）
 *   +0x1C = 屏宽副本     +0x20 = 屏高副本
 *   +0x24 = GetBaseLayerBuffer()（真机经 IDisplay+0x10 中转）
 * 另外 New 会把同一缓冲镜像到 IDisplay+0x10、把 IDisplay+0x04 置 1。
 * 层 0 终驻、永不被 FreeAllLayer 释放（RE 从 i=1 起循环）。
 * 已建立（载荷 +0x24 非 0）时直接返回 1，不重复写。
 * 返回 1 成功、0 失败。 */
int zm_layer_init_base(uc_engine *uc, uint32_t display);

typedef struct {
	uint32_t fmt;			 /* +0x00 ZMCF 色格式枚举（1=RGB565，2/3/4=32bit）；
								基础层的这一格 = GetBaseLayerDepth() 的返回值 */
	uint32_t x, y;			 /* +0x04 / +0x08 */
	uint32_t w;				 /* +0x0C 宽（兼 pitch） */
	uint32_t h;				 /* +0x10 高 */
	uint32_t cx, cy, cw, ch; /* +0x14/+0x18/+0x1C/+0x20 裁剪区 */
	uint32_t buf;			 /* +0x24 像素缓冲指针 */
	/* RE：sub_285D8 里
	 *   if (*(层+0x2C) != 0) 走 mask 函数，否则走 copy 函数   → +0x2C 是"启用透明"
	 *   v12 = RGB565(*(层+0x30))                              → +0x30 才是透明色
	 * 这也修正了 SetTransColor(a2,a3) 的语义：a2=启用标志，a3=颜色。 */
	uint32_t tenable; /* +0x2C 非 0 = 启用透明色 */
	uint32_t tcolor;  /* +0x30 透明色（ARGB） */
} zm_layer_t;

/* 读第 idx 层。成功（该层存在且有缓冲）返回 0，否则 -1。 */
int zm_layer_get(uc_engine *uc, uint32_t display, uint32_t idx, zm_layer_t *out);

/* +0x0C CreateLayer(display, idx, rect, fmt)
 * idx ∈ [1,15]；rect = {x,y,w,h}；fmt ∈ [1,4]。
 * 自己分配层缓冲并清零，填满层结构；该层已存在（+0x24 != 0）返回 -8。
 * 返回 0 成功、-4 参数错、-2 分配失败、-8 已存在。 */
uint32_t zm_layer_CreateLayer(
	uc_engine *uc, uint32_t display, uint32_t idx, uint32_t rect_ptr, uint32_t fmt);

/* +0x1C GetLayerInfo(display, idx, out)：把层项载荷 52 字节拷给调用方。
 * idx 允许 0..15（固件只校验 >0xF），层无缓冲返回 -4。 */
uint32_t zm_layer_GetLayerInfo(uc_engine *uc, uint32_t display, uint32_t idx, uint32_t out_ptr);

/* +0x30 SetActiveLayer(display, idx)：层无缓冲返回 -4。 */
uint32_t zm_layer_SetActiveLayer(uc_engine *uc, uint32_t display, uint32_t idx);

/* 合成顺序：按层号 0,1,2,... 依次叠加。
 * RE 里 ZMAEE_IDisplay_Update 传给 UpdateEx 的列表曾见 {0,1,2,3}，
 * 但 CreateLayerExt 允许 idx ∈ [0,15]，所以这里放宽到 16：
 * 多出来的层只是在 zm_layer_get 里被跳过，代价可忽略，
 * 但能避免"applet 建了层却永远不参与合成"。 */
#define ZM_LAYER_COMPOSITE_MAX 16

#endif /* ZM_LAYER_H */

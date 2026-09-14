#ifndef ZM_DISPLAY_H
#define ZM_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/* ZMAEE IDisplay / IBitmap 原生虚表（含 SDL2 渲染后端）。
 * 虚表布局与槽偏移严格按逆向贴出的 g_aee_display_vtbl / g_aee_bitmap_vtbl。
 * display 为全局单例（queryInterface(0x1000005) 返回），bitmap 由
 * IDisplay.CreateBitmap/LoadBitmap 创建（此处用 BITMAP 单例模板）。
 * 旧 GFX/GFX_VT 是同一张表的早期误命名，已并入此处。
 *
 * 渲染后端：SDL2 + SDL_ttf。
 *   - 画布大小由 AppHeader.ScreenW / ScreenH 决定（zm_display_init）
 *   - 颜色格式 ARGB8888（0xAARRGGBB），与 applet 一致
 *   - 以一张 ARGB8888 的 RenderTarget 纹理作为持久画布
 *
 * 参数布局说明：IDisplay 方法 r0=this（display 对象），真实绘制参数从 r1 起；
 * 实测过的槽按 applet 真实调用行为实现，其余槽接 zm_display_stub 仅记录
 * 日志、返回 0，保证任何槽被调用都不会落到 "非法的外部调用" 而卡死。
 */

/* -------------------- 生命周期 -------------------- */

/* 初始化 SDL2 渲染（窗口、渲染器、画布、字体子系统）。
 * screen_w / screen_h 取自 AppHeader，作为窗口与画布尺寸。
 * 成功返回 0，失败返回 -1。 */
int zm_display_init(void);

/* 取当前画布尺寸（供其它模块初始化客户机可见的上下文结构用）。
 * 未初始化时返回 0 并把 *w/*h 置 0。 */
void zm_display_size(int *w, int *h);

/* 释放 SDL 资源（窗口、渲染器、画布、字体） */
void zm_display_shutdown(void);

/* 事件循环：阻塞直到用户关窗（SDL_QUIT）或超时（ms，0=不限时）。
 * applet 没有主循环，由模拟器在 TR_enter_event_loop 里调用本函数代跑。
 * 返回 false → 模拟应结束；返回 true → 已派发点击，由调用者让模拟器
 * 继续执行 handler。 */
bool zm_display_event_loop(void (*on_click)(uint32_t x, uint32_t y),
                           uint32_t timeout_ms);

/* -------------------- IDisplay 各槽 -------------------- */

/* 通用 stub：记录 offset 与参数，返回 0 */
uint32_t zm_display_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                         uint32_t r2, uint32_t r3);

uint32_t zm_display_AddRef(uc_engine *uc, uint32_t r0);
uint32_t zm_display_Release(uc_engine *uc, uint32_t r0);
uint32_t zm_display_GetMaxLayerCount(uc_engine *uc, uint32_t r0);
uint32_t zm_display_CreateLayer(uc_engine *uc, uint32_t off, uint32_t r0,
                                uint32_t r1, uint32_t r2, uint32_t r3);
/* +0x18：RE 为 ZMAEE_IDisplay_GetBaseLayerBuffer() —— 返回常驻的"基础层"
 * 缓冲指针（真机是全局 unk_64B60），背景应当画在这里。 */
uint32_t zm_display_GetBaseLayerBuffer(uc_engine *uc);

/* RE 的 ZMAEE_IDisplay_GetBaseLayerDepth()：读屏幕色深（真机 ctx+0x38 =
 * g_aee.screenDepth），不在 [24,32] 返回 1，否则查 9 项表（.rodata:0x5B3E4
 * = {2,1,1,1,1,1,1,1,4}）→ 16bpp→1、24bpp→2、32bpp→4。 */
uint32_t zm_display_GetBaseLayerDepth(uc_engine *uc);

/* 上面那个函数的宿主等价物（不碰 uc），供 zm_layer_init_base 等使用。 */
uint32_t zm_display_base_depth(void);

/* 本模拟器上报/模拟的屏幕色深（位）。真机对应 g_aee.screenDepth，由
 * nativeAEEInit 从设备配置灌入；我们固定 16，即 g_aee 里那一格的值。 */
int zm_display_screen_depth(void);

/* 基础层缓冲地址（0 = 未分配），仅供诊断探针使用。 */
uint32_t zm_display_base_layer_addr(void);

uint32_t zm_display_CreateLayerExt(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_FreeLayer(uc_engine *uc, uint32_t r0, uint32_t r1);
uint32_t zm_display_GetLayerInfo(uc_engine *uc, uint32_t off, uint32_t r0,
                                 uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_SetLayerPosition(uc_engine *uc, uint32_t off, uint32_t r0,
                                     uint32_t r1, uint32_t r2, uint32_t r3);
/* +0x28 Update(display, x, y, w, h)：RE 00029D88 的薄封装，内部调
 * UpdateEx(display, {x,y,w,h}, 4, <默认层列表>)。以前只取 r0，xywh 被丢掉。 */
uint32_t zm_display_Update(uc_engine *uc, uint32_t display, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h);
uint32_t zm_display_GetActiveLayer(uc_engine *uc, uint32_t r0);
uint32_t zm_display_UnlockScreen(uc_engine *uc, uint32_t r0);
uint32_t zm_display_RegisterCustomFont(uc_engine *uc, uint32_t off, uint32_t r0,
                                       uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_GetFontWidth(uc_engine *uc, uint32_t r0, uint32_t r1);

/* 实测槽（旧 GFX 路径验证过的行为） */
/* +0x20：真机虚表里是 ZMAEE_IDisplay_SetActiveLayer(display, idx)。
 * 以前误认成 clear(color) —— 详见 zm_display.c 里的说明。 */
uint32_t zm_display_SetActiveLayer(uc_engine *uc, uint32_t display,
                                   uint32_t idx);
/* +0x2C：真机虚表里是 ZMAEE_IDisplay_UpdateEx(display, rect, count, layerList)。
 * 以前误认成 fillRect(rect_ptr)，只接一个参数、把层列表丢了。 */
uint32_t zm_display_UpdateEx(uc_engine *uc, uint32_t display, uint32_t rect_ptr,
                             uint32_t count, uint32_t list_ptr);
uint32_t zm_display_SelectFont(uc_engine *uc, uint32_t display, uint32_t font_idx); /* +0x40，真机虚表 SelectFont(this, idx) */
uint32_t zm_display_GetFontHeight(uc_engine *uc);  /* +0x48，真机虚表 GetFontHeight（实现待 RE 校准） */
uint32_t zm_display_MeasureString(uc_engine *uc, uint32_t disp, uint32_t str_ptr,
                                  uint32_t len, uint32_t width_out,
                                  uint32_t sp); /* +0x4C，真机虚表 MeasureString(disp, str, len, width, sp[metrics]) */
uint32_t zm_display_DrawText(uc_engine *uc, uint32_t rect_ptr, uint32_t text_ptr,
                             uint32_t text_len, uint32_t sp); /* +0x50 */
uint32_t zm_display_DrawRect(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                             uint32_t sp); /* +0x6C */
uint32_t zm_display_FillRect(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                             uint32_t sp); /* +0x70 */

uint32_t zm_display_SetTransColor(uc_engine *uc, uint32_t r0,
                                  uint32_t r1, uint32_t r2);
uint32_t zm_display_SetOpacity(uc_engine *uc, uint32_t off, uint32_t r0,
                               uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_SetClipRect(uc_engine *uc, uint32_t off, uint32_t r0,
                                uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_GetClipRect(uc_engine *uc, uint32_t off, uint32_t r0,
                                uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_SetPixel(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawLine(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawRoundRect(uc_engine *uc, uint32_t off, uint32_t r0,
                                  uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawCircle(uc_engine *uc, uint32_t off, uint32_t r0,
                               uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_FillCircle(uc_engine *uc, uint32_t off, uint32_t r0,
                               uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawArc(uc_engine *uc, uint32_t off, uint32_t r0,
                            uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_FillArc(uc_engine *uc, uint32_t off, uint32_t r0,
                            uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_FillGradientRect(uc_engine *uc, uint32_t off, uint32_t r0,
                                     uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_AlphaBlendRect(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawImage(uc_engine *uc, uint32_t off, uint32_t r0,
                              uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawBitmap(uc_engine *uc, uint32_t off, uint32_t r0,
                               uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawBitmapEx(uc_engine *uc, uint32_t off, uint32_t r0,
                                 uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawBitmapFrame(uc_engine *uc, uint32_t off, uint32_t r0,
                                    uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_CreateBitmap(uc_engine *uc, uint32_t r0, uint32_t r1,
                                 uint32_t r2, uint32_t r3);
uint32_t zm_display_LoadBitmap(uc_engine *uc, uint32_t r0, uint32_t r1);
uint32_t zm_display_CreateImage(uc_engine *uc, uint32_t off, uint32_t r0,
                                uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_BitBlt(uc_engine *uc, uint32_t off, uint32_t r0,
                           uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_Flatten(uc_engine *uc, uint32_t off, uint32_t r0,
                            uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_StretchBlt(uc_engine *uc, uint32_t off, uint32_t r0,
                               uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawAntialiasingLine(uc_engine *uc, uint32_t off, uint32_t r0,
                                         uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawWLine(uc_engine *uc, uint32_t off, uint32_t r0,
                              uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_GetDMLayerHdlr(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_RelevanceLayer(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_Refresh(uc_engine *uc);
uint32_t zm_display_DrawImageExt(uc_engine *uc, uint32_t off, uint32_t r0,
                                 uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawSysWallPaper(uc_engine *uc, uint32_t off, uint32_t r0,
                                     uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_DrawBorderText(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_PushAndSetAlphaLayer(uc_engine *uc, uint32_t off, uint32_t r0,
                                         uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_display_PopAndRestoreAlphaLayer(uc_engine *uc, uint32_t off,
                                            uint32_t r0, uint32_t r1,
                                            uint32_t r2, uint32_t r3);
uint32_t zm_display_RotateScreen(uc_engine *uc, uint32_t off, uint32_t r0,
                                 uint32_t r1, uint32_t r2, uint32_t r3);

/* -------------------- IBitmap 各槽 -------------------- */
uint32_t zm_bitmap_AddRef(uc_engine *uc, uint32_t r0);
uint32_t zm_bitmap_Release(uc_engine *uc, uint32_t r0);
uint32_t zm_bitmap_SetTransColor(uc_engine *uc, uint32_t r0, uint32_t r1);
uint32_t zm_bitmap_sub_25F78(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_bitmap_GetInfo(uc_engine *uc, uint32_t r0, uint32_t r1);
uint32_t zm_bitmap_sub_25F84(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t zm_bitmap_sub_25FF8(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3);

/* -------------------- 软件帧缓冲（宿主侧访问） --------------------
 *
 * applet 的所有绘制（含 BitBlt 区域搬运、GDI_Surface 贴图）都落在一块
 * ARGB8888 的软件帧缓冲上，commit 时整块上传给 SDL。
 * 下面两个接口供其它模块（如 zm_image 贴 16bpp GDI_Surface）直接写入。 */
uint32_t *zm_fb_buffer(int *w, int *h);       /* 取帧缓冲（未初始化返回 NULL） */
void zm_fb_write(int x, int y, uint32_t argb); /* 写像素（自动裁剪越界） */

#endif /* ZM_DISPLAY_H */

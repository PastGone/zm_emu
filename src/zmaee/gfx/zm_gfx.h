#ifndef __ZM_GFX_H__
#define __ZM_GFX_H__

// -------------------- gfx 相关 trap 处理函数声明 --------------------
// 基于 SDL2 + SDL_ttf 的真实渲染实现：
//   - 画布大小由 AppHeader.ScreenW / ScreenH 决定（zm_gfx_init 传入）
//   - 颜色格式为 ARGB8888（0xAARRGGBB），与 applet 一致
//   - 以一张 ARGB8888 的 RenderTarget 纹理作为持久画布，
//     fb_commit 时把画布拷到屏幕并 Present
#include <stdint.h>
#include <unicorn/unicorn.h>

/* 初始化 SDL 窗口 / 渲染器 / 画布 / 字体。
 * screen_w / screen_h 取自 AppHeader，作为窗口与画布尺寸。
 * 成功返回 0，失败返回 -1。 */
int zm_gfx_init(uint32_t screen_w, uint32_t screen_h);

/* 释放 SDL 资源（窗口、渲染器、画布、字体） */
void zm_gfx_shutdown(void);

/* 保持窗口显示直到用户关闭或超时（ms）。
 * applet 只跑一次 init、无事件循环，故 uc_emu_start 返回后用此函数
 * 让最后一帧停留以便观察。timeout_ms=0 表示不限时（直到关窗）。 */
void zm_gfx_hold(uint32_t timeout_ms);

/**
 * @brief 事件循环：保持窗口显示，处理关闭与鼠标点击
 * @param on_click  鼠标按下回调（传入画布坐标）；NULL 则不处理点击
 * @param timeout_ms 0=直到窗口关闭；>0 停留指定毫秒后返回
 *
 * on_click 内部通常会调用 uc_emu_start 把点击转发给 applet 的触摸 handler，
 * 注意该回调执行期间会阻塞 SDL 事件处理。
 */
void zm_gfx_event_loop(void (*on_click)(uint32_t x, uint32_t y),
                       uint32_t timeout_ms);

/* gfx.clear：以 color 清屏。r1=color */
uint32_t zm_gfx_clear(uc_engine *uc, uint32_t color);

/* gfx.fillRect：填充矩形。r1=rect_ptr（当前为空实现） */
uint32_t zm_gfx_fillRect(uc_engine *uc, uint32_t rect_ptr);

/* gfx.commit：提交帧缓冲。 */
uint32_t zm_gfx_commit(uc_engine *uc);

/* gfx.drawText：绘制文本。
 * r1=rect_ptr, r2=text_ptr, r3=text_len, sp=color, sp+8=font_size */
uint32_t zm_gfx_drawText(uc_engine *uc, uint32_t rect_ptr, uint32_t text_ptr,
                         uint32_t text_len, uint32_t sp);

/* gfx.drawRect：绘制矩形边框。
 * r1=x, r2=y, r3=w, sp=h, sp+4=color */
uint32_t zm_gfx_drawRect(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                         uint32_t sp);

/* gfx.fillRect2：填充矩形。
 * r1=x, r2=y, r3=w, sp=h, sp+4=color */
uint32_t zm_gfx_fillRect2(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                          uint32_t sp);

/* ---- 00000405.app：GFX vtable 缺失槽 stub ---- */
uint32_t zm_gfx_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                     uint32_t r2, uint32_t r3);
uint32_t zm_gfx_get_width(uc_engine *uc); /* GFX_VT[0x48]：返回屏幕宽度 */

/* GFX_VT[0x4C]：measureChar(gfx, char_ptr, count, width_out, metrics_buf)
 * sub_802EC 文本布局用它测量字符宽度；stub 写 0 到 *width_out 并返回 0。
 * 后续可用 TTF_SizeUTF8 实现真实测量。 */
uint32_t zm_gfx_measure_char(uc_engine *uc, uint32_t gfx, uint32_t char_ptr,
                             uint32_t count, uint32_t width_out);

#endif

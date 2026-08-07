#ifndef ZM_GFX_H
#define ZM_GFX_H

// -------------------- gfx 相关 trap 处理函数声明 --------------------
// 基于 SDL2 + SDL_ttf 的真实渲染实现：
//   - 画布大小由 AppHeader.ScreenW / ScreenH 决定
//   - 颜色格式为 ARGB8888（0xAARRGGBB），与 applet 一致
//   - 以一张 ARGB8888 的 RenderTarget 纹理作为持久画布，
//     fb_commit 时把画布拷到屏幕并 Present
//   - 无头模式（g_headless）下使用 SDL "dummy" 视频驱动 + 软件渲染，
//     照样能完成全部绘制，可用 zm_gfx_save_bmp 把结果落盘校验
#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/* 初始化 SDL 窗口 / 渲染器 / 画布 / 字体。
 * 尺寸取自 AppHeader。成功返回 0，失败返回 -1。 */
int zm_gfx_init(void);

/* 释放 SDL 资源（窗口、渲染器、画布、字体） */
void zm_gfx_shutdown(void);

/* 是否已成功初始化（渲染可用） */
bool zm_gfx_ready(void);

/* 把画布内容拷到窗口并 Present（无头模式下也安全） */
void zm_gfx_present(void);

/* 把当前画布另存为 BMP，用于无头模式下校验渲染结果。
 * 成功返回 0，失败返回 -1。 */
int zm_gfx_save_bmp(const char *path);

/* 统计画布上的非背景像素数量与去重颜色数，用于自动化校验"确实画了东西"。
 * 任一出参可为 NULL。成功返回 0。 */
int zm_gfx_canvas_stats(uint32_t *out_nonzero_px, uint32_t *out_distinct_colors);

/* 保持窗口显示直到用户关闭或超时（ms）。timeout_ms=0 表示不限时。 */
void zm_gfx_hold(uint32_t timeout_ms);

/**
 * @brief SDL 事件循环：保持窗口显示，处理关闭与鼠标点击
 * @param on_click  鼠标按下回调（传入画布坐标）；NULL 则不处理点击
 * @param timeout_ms 0=直到窗口关闭；>0 停留指定毫秒后返回
 * @return true  已派发点击事件，调用者应让模拟器继续执行 handler
 * @return false 用户关窗(SDL_QUIT)或超时，调用者应停止模拟器
 */
bool zm_gfx_event_loop(void (*on_click)(uint32_t x, uint32_t y),
                       uint32_t timeout_ms);

/* 累计绘制操作次数（用于摘要与"确实画了东西"的判定） */
uint32_t zm_gfx_draw_ops(void);

/* gfx.fillRect：填充矩形。r1=rect_ptr（当前为空实现） */
uint32_t zm_gfx_fillRect(uc_engine *uc, uint32_t rect_ptr);

/* gfx.commit：提交帧缓冲。 */
uint32_t zm_gfx_commit(uc_engine *uc);

/* gfx.drawText：绘制文本。
 * r1=rect_ptr, r2=text_ptr, r3=text_len, sp=color, sp+8=font_size */
uint32_t zm_gfx_drawText(uc_engine *uc, uint32_t rect_ptr, uint32_t text_ptr,
                         uint32_t text_len, uint32_t sp);

/* gfx.drawRect：绘制矩形边框。r1=x, r2=y, r3=w, sp=h, sp+4=color */
uint32_t zm_gfx_drawRect(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                         uint32_t sp);

/* gfx.fillRect2：填充矩形。r1=x, r2=y, r3=w, sp=h, sp+4=color */
uint32_t zm_gfx_fillRect2(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                          uint32_t sp);

/* gfx.fillRect5C：00000440 的 fillRect(x,y,w,h)，无颜色参数，暂用白色 */
uint32_t zm_gfx_fillRect5C(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                           uint32_t sp);

/* gfx.vtB4：三段式横条 blit（00000001 进度条）。
 * r1=xy_ptr{8B}, r2=imginfo_ptr{w,h}, r3=param_ptr{srcX,srcY,segW,color} */
uint32_t zm_gfx_vtB4(uc_engine *uc, uint32_t obj, uint32_t xy_ptr,
                     uint32_t info_ptr, uint32_t param_ptr);

/* gfx.DrawLine：00000405 画内容矩形边框。
 * r1=x1, r2=y1, r3=x2, sp=y2, sp+4=color */
uint32_t zm_gfx_draw_line(uc_engine *uc, uint32_t x1, uint32_t y1,
                          uint32_t x2, uint32_t sp);

/* gfx.vt44：00000405 无参 display 操作，no-op 返 0 */
uint32_t zm_gfx_vt44(uc_engine *uc);

/* 通用 stub：记录命中的 GFX vtable 偏移与参数后返回 0 */
uint32_t zm_gfx_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                     uint32_t r2, uint32_t r3);

/* GFX_VT[0x48]：返回屏幕宽度 */
uint32_t zm_gfx_get_width(uc_engine *uc);

/* GFX_VT[0x4C]：measureChar(gfx, char_ptr, count, width_out)
 * 用 TTF_GlyphMetrics 做真实测量，写入 *width_out，返回 0。 */
uint32_t zm_gfx_measure_char(uc_engine *uc, uint32_t gfx, uint32_t char_ptr,
                             uint32_t count, uint32_t width_out);

#endif

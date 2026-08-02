#include "zm_gfx.h"

#include "../../emu.h"
#include "../../log/log.h"
#include "../../tool/uc_helper.h"
#include "unicorn/unicorn.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <stdlib.h>

/* ---------- 渲染器（SDL2 + SDL_ttf） ----------
 * 设计要点：
 *  - 一张 ARGB8888 的 RenderTarget 纹理 g_canvas 作为持久画布，
 *    fb_clear / fb_fill_rect / fb_draw_rect / fb_draw_text 都把内容画到它上面；
 *  - fb_commit 时把 g_canvas 拷到屏幕并 RenderPresent；
 *  - 颜色格式：applet 传 0xAARRGGBB，与 SDL_PIXELFORMAT_ARGB8888 一致；
 *  - 文本用 TTF 渲染后 blit 到画布，alpha 混合交给 SDL；
 *  - commit 时顺便 PollEvent，让窗口可正常刷新 / 关闭。
 */

/* 系统中可用的拉丁字体；applet(Soundboard) 只渲染数字 1~25，拉丁字体足够 */
#define ZM_FONT_PATH "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"

static SDL_Window *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static SDL_Texture *g_canvas = NULL; /* ARGB8888 RenderTarget，作为画布 */
static int g_w = 0;
static int g_h = 0;

/* 字体按 font_size 缓存，size 变化时重新打开 */
static TTF_Font *g_font = NULL;
static int g_font_size = 0;

/* 把 0xAARRGGBB 转成 SDL_Color（保留 alpha） */
static SDL_Color to_sdl_color(uint32_t argb) {
  SDL_Color c;
  c.a = (uint8_t)((argb >> 24) & 0xFF);
  c.r = (uint8_t)((argb >> 16) & 0xFF);
  c.g = (uint8_t)((argb >> 8) & 0xFF);
  c.b = (uint8_t)(argb & 0xFF);
  return c;
}

/* 设置当前绘制色（强制不透明，避免 clear/fill 叠加产生意外透明） */
static void set_draw_color(uint32_t argb) {
  SDL_Color c = to_sdl_color(argb);
  SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, 0xFF);
}

/* 确保 render target 为画布 */
static void target_canvas(void) { SDL_SetRenderTarget(g_ren, g_canvas); }

/* 根据 font_size 打开（或复用）字体 */
static TTF_Font *get_font(int font_size) {
  if (font_size <= 0)
    font_size = 16;
  /* 00000405.app 传入的 font_size 可能是 font_id(1,2)而非像素值；
   * 小于 8 时视为 font_id，映射到可读的像素大小。 */
  if (font_size < 8)
    font_size = 14;
  if (g_font && g_font_size == font_size)
    return g_font;
  if (g_font) {
    TTF_CloseFont(g_font);
    g_font = NULL;
  }
  g_font = TTF_OpenFont(ZM_FONT_PATH, font_size);
  if (!g_font) {
    log_error("TTF_OpenFont failed: %s", TTF_GetError());
    return NULL;
  }
  g_font_size = font_size;
  return g_font;
}

/* ---------- 画布操作 ---------- */

static void fb_clear(uint32_t color) {
  if (!g_ren)
    return;
  target_canvas();
  set_draw_color(color);
  SDL_RenderClear(g_ren);
}

static void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
  if (!g_ren)
    return;
  target_canvas();
  set_draw_color(color);
  SDL_Rect r = {x, y, w, h};
  SDL_RenderFillRect(g_ren, &r);
}

static void fb_draw_rect(int x, int y, int w, int h, uint32_t color) {
  if (!g_ren)
    return;
  target_canvas();
  set_draw_color(color);
  SDL_Rect r = {x, y, w, h};
  SDL_RenderDrawRect(g_ren, &r);
}

/* 在 rect_ptr 指向的矩形 {x,y,w,h} 内居中绘制文本 */
static void fb_draw_text(uc_engine *uc, uint32_t rect_ptr, const char *text,
                         uint32_t color, int font_size) {
  if (!g_ren || !rect_ptr || !text || !text[0])
    return;

  int rx = (int)uc_read32(uc, rect_ptr);
  int ry = (int)uc_read32(uc, rect_ptr + 4);
  int rw = (int)uc_read32(uc, rect_ptr + 8);
  int rh = (int)uc_read32(uc, rect_ptr + 12);

  TTF_Font *font = get_font(font_size);
  if (!font)
    return;

  SDL_Color fg = to_sdl_color(color);
  /* Blended 抗锯齿；alpha 由字体光栅化决定，RGB 取自 color */
  SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, fg);
  if (!surf) {
    log_error("TTF_RenderUTF8_Blended failed: %s", TTF_GetError());
    return;
  }
  SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
  int tw = surf->w;
  int th = surf->h;
  SDL_FreeSurface(surf);
  if (!tex)
    return;

  /* 居中放置在矩形内；超出则裁剪到矩形范围内 */
  SDL_Rect dst;
  dst.w = tw;
  dst.h = th;
  dst.x = rx + (rw - tw) / 2;
  dst.y = ry + (rh - th) / 2;

  target_canvas();
  /* 限制在 rect 内，避免文本溢出按钮 */
  SDL_Rect clip = {rx, ry, rw, rh};
  SDL_RenderSetClipRect(g_ren, &clip);
  SDL_RenderCopy(g_ren, tex, NULL, &dst);
  SDL_RenderSetClipRect(g_ren, NULL);
  SDL_DestroyTexture(tex);
}

static void fb_commit(void) {
  if (!g_ren)
    return;
  /* 把画布拷到默认目标（窗口） */
  SDL_SetRenderTarget(g_ren, NULL);
  SDL_RenderCopy(g_ren, g_canvas, NULL, NULL);
  SDL_RenderPresent(g_ren);

  /* 处理一下事件，避免窗口卡死无响应 */
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) {
      /* 用户关闭窗口：直接退出进程（applet 一般不响应，强制结束） */
      log_info("SDL 窗口已关闭，退出模拟");
      exit(0);
    }
  }
}

/* ---------- 生命周期 ---------- */

int zm_gfx_init() {
  if (g_win)
    return 0; /* 已初始化 */

  if (g_header.ScreenW == 0)
    g_header.ScreenW = 240;
  if (g_header.ScreenH == 0)
    g_header.ScreenH = 240;
  g_w = (int)g_header.ScreenW;
  g_h = (int)g_header.ScreenH;

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    log_error("SDL_Init failed: %s", SDL_GetError());
    return -1;
  }
  if (TTF_Init() != 0) {
    log_error("TTF_Init failed: %s", TTF_GetError());
    return -1;
  }

  char window_title[128];
  char *ext_char = " (zm_emu)";
  snprintf(window_title, sizeof(window_title), "%s%s", g_header.AppName,
           ext_char);

  g_win =
      SDL_CreateWindow(window_title, SDL_WINDOWPOS_UNDEFINED,
                       SDL_WINDOWPOS_UNDEFINED, g_w, g_h, SDL_WINDOW_RESIZABLE);
  if (!g_win) {
    log_error("SDL_CreateWindow failed: %s", SDL_GetError());
    return -1;
  }
  /* 画布很小，优先尝试加速+目标纹理；失败则回退到软件渲染（无头环境） */
  g_ren = SDL_CreateRenderer(
      g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
  if (!g_ren) {
    log_warn("SDL_CreateRenderer(accel) failed: %s, 尝试软件渲染",
             SDL_GetError());
    g_ren = SDL_CreateRenderer(
        g_win, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
  }
  if (!g_ren) {
    log_warn("SDL_CreateRenderer(software+target) failed: %s, 尝试纯软件",
             SDL_GetError());
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
  }
  if (!g_ren) {
    log_error("SDL_CreateRenderer failed: %s", SDL_GetError());
    return -1;
  }

  SDL_RenderSetLogicalSize(g_ren, g_w, g_h);
  g_canvas = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_TARGET, g_w, g_h);
  if (!g_canvas) {
    log_error("SDL_CreateTexture(canvas) failed: %s", SDL_GetError());
    return -1;
  }

  /* 画布初始清成黑色 */
  target_canvas();
  SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 0xFF);
  SDL_RenderClear(g_ren);

  log_info("zm_gfx_init: 窗口 %dx%d 已创建", g_w, g_h);
  return 0;
}

void zm_gfx_shutdown(void) {
  if (g_font) {
    TTF_CloseFont(g_font);
    g_font = NULL;
  }
  if (g_canvas) {
    SDL_DestroyTexture(g_canvas);
    g_canvas = NULL;
  }
  if (g_ren) {
    SDL_DestroyRenderer(g_ren);
    g_ren = NULL;
  }
  if (g_win) {
    SDL_DestroyWindow(g_win);
    g_win = NULL;
  }
  TTF_Quit();
  SDL_Quit();
}

void zm_gfx_hold(uint32_t timeout_ms) {
  if (!g_win)
    return;
  /* applet 的 commit 发生在 fillRect2/drawText
   * 之前（clear→commit→画→无commit）， 故 commit 时显示的是黑屏；这里在进入
   * hold 前 present 一次最终画布， 让 fillRect2/drawText 绘制的内容得以显示。
   */
  SDL_SetRenderTarget(g_ren, NULL);
  SDL_RenderCopy(g_ren, g_canvas, NULL, NULL);
  SDL_RenderPresent(g_ren);
  SDL_Event e;
  Uint32 start = SDL_GetTicks();
  for (;;) {
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT)
        return;
    }
    if (timeout_ms != 0 && SDL_GetTicks() - start >= timeout_ms)
      return;
    SDL_Delay(16);
  }
}

void zm_gfx_event_loop(void (*on_click)(uint32_t x, uint32_t y),
                       uint32_t timeout_ms) {
  if (!g_win)
    return;
  /* present 最终画布（init 绘制内容）让用户看到界面 */
  SDL_SetRenderTarget(g_ren, NULL);
  SDL_RenderCopy(g_ren, g_canvas, NULL, NULL);
  SDL_RenderPresent(g_ren);

  /* 窗口像素坐标 -> 画布坐标的缩放（窗口尺寸即画布尺寸时为 1:1） */
  int win_w = g_w, win_h = g_h;
  SDL_GetWindowSize(g_win, &win_w, &win_h);

  SDL_Event e;
  Uint32 start = SDL_GetTicks();
  for (;;) {
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT)
        return;
      if (e.type == SDL_MOUSEBUTTONDOWN && on_click) {
        uint32_t cx = (win_w > 0) ? (uint32_t)(e.button.x * g_w / win_w)
                                  : (uint32_t)e.button.x;
        uint32_t cy = (win_h > 0) ? (uint32_t)(e.button.y * g_h / win_h)
                                  : (uint32_t)e.button.y;
        /* 回调内会 uc_emu_start 调用 applet handler，期间阻塞事件处理 */
        on_click(cx, cy);
        /* applet 的 touch handler 通常不重绘，但保险起见 present 一次 */
        SDL_SetRenderTarget(g_ren, NULL);
        SDL_RenderCopy(g_ren, g_canvas, NULL, NULL);
        SDL_RenderPresent(g_ren);
      }
    }
    if (timeout_ms != 0 && SDL_GetTicks() - start >= timeout_ms)
      return;
    SDL_Delay(16);
  }
}

/* ---------- gfx trap 处理函数 ---------- */

/* gfx.clear：以 color 清屏 */
uint32_t zm_gfx_clear(uc_engine *uc, uint32_t color) {
  (void)uc;
  fb_clear(color);
  return 0;
}

/* gfx.fillRect：r1=rect_ptr。
 * 注意：applet 在绘制末尾调用 fillRect(全屏rect, 1, &0)，语义不明
 * （疑似 invalidate / 带透明度混合，颜色=0 透明）。main.txt.c 也做成
 * 空实现。为避免用黑色覆盖整张画面，这里保持空实现。 */
uint32_t zm_gfx_fillRect(uc_engine *uc, uint32_t rect_ptr) {
  (void)uc;
  (void)rect_ptr;
  return 0;
}

/* gfx.commit：提交帧缓冲 */
uint32_t zm_gfx_commit(uc_engine *uc) {
  (void)uc;
  fb_commit();
  return 0;
}

/* gfx.drawText：绘制文本
 * r1=rect_ptr, r2=text_ptr, r3=text_len, sp=color, sp+8=font_size */
uint32_t zm_gfx_drawText(uc_engine *uc, uint32_t rect_ptr, uint32_t text_ptr,
                         uint32_t text_len, uint32_t sp) {
  char text[256];
  if (text_ptr && text_len) {
    uint32_t n = text_len < (uint32_t)sizeof(text) - 1
                     ? text_len
                     : (uint32_t)sizeof(text) - 1;
    uc_mem_read(uc, text_ptr, text, n);
    text[n] = '\0';
  } else {
    text[0] = '\0';
  }
  uint32_t color = uc_read32(uc, sp);
  uint32_t font_sz = uc_read32(uc, sp + 8);
  fb_draw_text(uc, rect_ptr, text, color, (int)font_sz);
  return 0;
}

/* gfx.drawRect：绘制矩形边框
 * r1=x, r2=y, r3=w, sp=h, sp+4=color */
uint32_t zm_gfx_drawRect(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                         uint32_t sp) {
  int h = (int)uc_read32(uc, sp);
  uint32_t color = uc_read32(uc, sp + 4);
  fb_draw_rect((int)x, (int)y, (int)w, h, color);
  return 0;
}

/* gfx.fillRect2：填充矩形
 * r1=x, r2=y, r3=w, sp=h, sp+4=color */
uint32_t zm_gfx_fillRect2(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                          uint32_t sp) {
  int h = (int)uc_read32(uc, sp);
  uint32_t color = uc_read32(uc, sp + 4);
  fb_fill_rect((int)x, (int)y, (int)w, h, color);
  return 0;
}

/* ---- 00000405.app：GFX vtable 缺失槽 stub ----
 * 这些偏移在 applet 绘制流程中被调用但功能未知，暂返回 0。
 * 后续可根据 applet 行为逐步实现。
 *   0x18: 疑似 setClipRect / setViewport
 *   0x34: 疑似 beginDraw / resetGfxState
 *   0x38: 疑似 endDraw / flush
 *   0x44: 疑似 setFont / setColor
 *   0x54: 疑似 drawLine
 *   0x68: 疑似 drawImage / drawBitmap
 *   0x94: 未知
 *   0xA4: 未知
 *   0xB0: 未知
 */
uint32_t zm_gfx_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                     uint32_t r2, uint32_t r3) {
  (void)uc;
  (void)r0;
  (void)r1;
  (void)r2;
  (void)r3;
  log_info("stub gfx[0x%X] r0=%u r1=%u r2=%u r3=%u", off, r0, r1, r2, r3);
  return 0;
}

/* GFX_VT[0x48]：返回屏幕宽度。
 * sub_8062C 用返回值+8 作为文本布局宽度；
 * sub_80790 用返回值+a2 作为文本区域宽度。 */
uint32_t zm_gfx_get_width(uc_engine *uc) {
  (void)uc;
  log_info("gfx[0x48] getWidth -> %u", g_w);
  return g_w;
}

/* GFX_VT[0x4C]：measureChar(gfx, char_ptr, count, width_out, metrics_buf)
 * sub_802EC 文本布局循环中调用，用于逐字符测量宽度并推进排版游标。
 *   r0=gfx, r1=char_ptr(指向 uint16 字符码), r2=count, r3=width_out(int*),
 *   sp[0]=metrics_buf(4B)
 * stub：向 *width_out 写一个固定宽度（取 font 默认值），避免文本叠在一起。
 * 返回 0。 */
uint32_t zm_gfx_measure_char(uc_engine *uc, uint32_t gfx, uint32_t char_ptr,
                             uint32_t count, uint32_t width_out) {
  (void)gfx;
  (void)char_ptr;
  (void)count;
  /* 写入固定字符宽度（约 font_size 的 60%），使文本不致全叠在同一位置 */
  if (width_out)
    uc_write32(uc, width_out, 8);
  return 0;
}

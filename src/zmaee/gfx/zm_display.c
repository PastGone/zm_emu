#include "zm_display.h"

#include "../../emu.h"
#include "../../event.h"
#include "../../log/log.h"
#include "../../trap.h" /* getArg：第 5 个及以后的参数 */
#include "../../tool/uc_helper.h"
#include "../runtime/timer/zm_timer.h" /* zm_timer_poll：到期定时器检查 */
#include "zm_image.h" /* IImage/IBitmap 像素取用（DrawImage/DrawBitmap） */

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <png.h> /* 调试截图（fb_save_png） */

#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * ZMAEE IDisplay / IBitmap 原生虚表处理函数（含 SDL2 渲染后端）
 *
 * 虚表布局严格按逆向贴出的 g_aee_display_vtbl（.data:0x63E10，58 槽）与
 * g_aee_bitmap_vtbl（.data:0x63DF4，7 槽）。display 为全局单例，经
 * queryInterface(0x1000005) 取得；bitmap 由 IDisplay.CreateBitmap/LoadBitmap
 * 创建（本实现用 BITMAP 单例模板，真实多实例后续扩展）。
 * 旧 GFX/GFX_VT 是同一张表的早期误命名（实测偏移 +0x50 drawText、
 * +0x6C drawRect、+0x70 fillRect 与本表完全吻合），已并入 display。
 *
 * 渲染器（SDL2 + SDL_ttf）设计要点：
 *  - 一张 ARGB8888 的 RenderTarget 纹理 g_canvas 作为持久画布，
 *    fb_clear / fb_fill_rect / fb_draw_rect / fb_draw_text 都把内容画到它上面；
 *  - fb_commit 时把 g_canvas 拷到屏幕并 RenderPresent；
 *  - 颜色格式：applet 传 0xAARRGGBB，与 SDL_PIXELFORMAT_ARGB8888 一致；
 *  - 文本用 TTF 渲染后 blit 到画布，alpha 混合交给 SDL；
 *  - commit 时顺便 PollEvent，让窗口可正常刷新 / 关闭。
 *
 * 原则：任何槽被调用都不应落到 "非法的外部调用" 而卡死 pause_console。
 *   - 实测过行为的槽（clear/fillRectR/commit/getWidth/measureChar/
 *     DrawText/DrawRect/FillRect/Update/Refresh）按真实行为实现；
 *   - 返回对象的（CreateBitmap/LoadBitmap）返回 BITMAP 单例；
 *   - 其余接 zm_display_stub：仅记录日志、返回 0。
 *
 * 注意：IDisplay 方法 r0 = this（display 对象），真实绘制参数从 r1 起；
 * 未实测槽的参数布局待后续 RE 校准。
 * ========================================================================= */

/* ---------- 渲染后端状态 ---------- */

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

/* ---------- 宿主软件帧缓冲 ----------
 *
 * 为什么不用 SDL_Render* 直接画：applet 的绘制模型是"往当前 layer 里画"，
 * 并且大量使用 IDisplay::BitBlt 做**画面内区域搬运**（滚动/合成），
 * 还有 sprite 的镜像翻转。这些用 SDL 的 RenderTarget 很难表达（读回像素
 * 慢且丢精度）。改成宿主侧一块 ARGB8888 数组作为唯一真相，commit 时整体
 * 上传到纹理并 present——240x320 只有 300KB，代价可忽略。
 */
static uint32_t *g_fb = NULL;
static int g_fb_w = 0, g_fb_h = 0;

/* argb8888 → rgb565（写客户机层缓冲用） */
static inline uint16_t to_rgb565(uint32_t argb) {
  unsigned r = (argb >> 16) & 0xFFu, g = (argb >> 8) & 0xFFu,
           b = argb & 0xFFu;
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline void fb_px(int x, int y, uint32_t argb) {
  if ((unsigned)x >= (unsigned)g_fb_w || (unsigned)y >= (unsigned)g_fb_h)
    return;
  if (g_fb)
    g_fb[(size_t)y * (size_t)g_fb_w + (size_t)x] = argb;

  /* 同步写客户机可见的层缓冲（RGB565）：
   * applet 会通过 GetLayerInfo 拿到这块缓冲的指针，并直接读它做后续
   * 合成（例如把层内容再 BitBlt 到别处），所以两边必须一致。
   * 层尺寸与屏幕一致，坐标在范围内时才写。 */
  if (g_uc && (unsigned)x < (unsigned)LAYER_W &&
      (unsigned)y < (unsigned)LAYER_H) {
    uint16_t c = to_rgb565(argb);
    uc_mem_write(g_uc, LAYER_BUF + ((uint32_t)y * LAYER_W + (uint32_t)x) * 2u,
                 &c, sizeof(c));
  }
}

static inline uint32_t fb_get(int x, int y) {
  if (!g_fb || (unsigned)x >= (unsigned)g_fb_w ||
      (unsigned)y >= (unsigned)g_fb_h)
    return 0;
  return g_fb[(size_t)y * (size_t)g_fb_w + (size_t)x];
}

uint32_t *zm_fb_buffer(int *w, int *h) {
  if (w)
    *w = g_fb_w;
  if (h)
    *h = g_fb_h;
  return g_fb;
}

void zm_fb_write(int x, int y, uint32_t argb) { fb_px(x, y, argb); }

static void fb_clear(uint32_t color) {
  if (!g_fb)
    return;
  for (size_t i = 0, n = (size_t)g_fb_w * (size_t)g_fb_h; i < n; i++)
    g_fb[i] = color;
}

static void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
  if (!g_fb || w <= 0 || h <= 0)
    return;
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w > g_fb_w ? g_fb_w : x + w;
  int y1 = y + h > g_fb_h ? g_fb_h : y + h;
  for (int yy = y0; yy < y1; yy++) {
    uint32_t *row = g_fb + (size_t)yy * (size_t)g_fb_w;
    for (int xx = x0; xx < x1; xx++)
      row[xx] = color;
  }
}

static void fb_draw_rect(int x, int y, int w, int h, uint32_t color) {
  if (!g_fb)
    return;
  for (int xx = x; xx < x + w; xx++) {
    fb_px(xx, y, color);
    fb_px(xx, y + h - 1, color);
  }
  for (int yy = y; yy < y + h; yy++) {
    fb_px(x, yy, color);
    fb_px(x + w - 1, yy, color);
  }
}

/* 在 rect_ptr 指向的矩形 {x,y,w,h} 内居中绘制文本。
 * 用 TTF 光栅化成 ARGB8888 表面后逐像素 alpha 混合进软件帧缓冲
 * （不再走 SDL_Render*，保证与 BitBlt 等操作共用同一块画布）。 */
static void fb_draw_text(uc_engine *uc, uint32_t rect_ptr, const char *text,
                         uint32_t color, int font_size) {
  if (!g_fb || !rect_ptr || !text || !text[0])
    return;

  int rx = (int)uc_read32(uc, rect_ptr);
  int ry = (int)uc_read32(uc, rect_ptr + 4);
  int rw = (int)uc_read32(uc, rect_ptr + 8);
  int rh = (int)uc_read32(uc, rect_ptr + 12);

  TTF_Font *font = get_font(font_size);
  if (!font)
    return;

  SDL_Color fg = to_sdl_color(color);
  SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, fg);
  if (!surf) {
    log_error("TTF_RenderUTF8_Blended failed: %s", TTF_GetError());
    return;
  }
  SDL_Surface *conv =
      SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ARGB8888, 0);
  SDL_FreeSurface(surf);
  if (!conv)
    return;

  int tw = conv->w, th = conv->h;
  int dx = rx + (rw - tw) / 2;
  int dy = ry + (rh - th) / 2;

  if (SDL_MUSTLOCK(conv))
    SDL_LockSurface(conv);
  for (int y = 0; y < th; y++) {
    int oy = dy + y;
    if (oy < ry || oy >= ry + rh)
      continue; /* 裁剪到 rect 内，避免文本溢出按钮 */
    const uint32_t *src =
        (const uint32_t *)((const uint8_t *)conv->pixels + (size_t)y * conv->pitch);
    for (int x = 0; x < tw; x++) {
      int ox = dx + x;
      if (ox < rx || ox >= rx + rw)
        continue;
      uint32_t s = src[x];
      unsigned a = (s >> 24) & 0xFF;
      if (a == 0)
        continue;
      if (a == 0xFF) {
        fb_px(ox, oy, 0xFF000000u | (s & 0x00FFFFFFu));
        continue;
      }
      uint32_t d = fb_get(ox, oy);
      unsigned sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
      unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
      unsigned r = (sr * a + dr * (255 - a)) / 255;
      unsigned g = (sg * a + dg * (255 - a)) / 255;
      unsigned b = (sb * a + db * (255 - a)) / 255;
      fb_px(ox, oy, 0xFF000000u | (r << 16) | (g << 8) | b);
    }
  }
  if (SDL_MUSTLOCK(conv))
    SDL_UnlockSurface(conv);
  SDL_FreeSurface(conv);
}

/* 绘制 RGBA8888 图像到帧缓冲（IImage / IBitmap 共用）。
 *
 * mode：applet 的"类型字节"（unk_1F714 表的索引），在 sub_50D8 / sub_51A0
 * 等函数里表现为对矩形做镜像/交换，因此这里把常见几种翻转折算进去：
 *   0 = 原样，1 = 垂直翻转，2 = 水平翻转，3 = 180°，
 *   4 = 转置，5 = 转置+垂直，6 = 转置+水平，7 = 转置+180°
 * alpha==0 的像素视为透明（PNG 自带 alpha；JPEG 全不透明）。
 * 另外兼容固件的 RGB565 透明色键 0xF81F（洋红）——部分 sprite 用它做抠图。
 */
static void fb_blit_rgba_mode(int x, int y, int w, int h, const uint8_t *rgba,
                              int mode) {
  if (!g_fb || !rgba || w <= 0 || h <= 0)
    return;
  for (int sy = 0; sy < h; sy++) {
    for (int sx = 0; sx < w; sx++) {
      int dx = sx, dy = sy;
      switch (mode & 7) {
      case 1: dy = h - 1 - sy; break;
      case 2: dx = w - 1 - sx; break;
      case 3: dx = w - 1 - sx; dy = h - 1 - sy; break;
      case 4: dx = sy; dy = sx; break;
      case 5: dx = h - 1 - sy; dy = sx; break;
      case 6: dx = sy; dy = w - 1 - sx; break;
      case 7: dx = h - 1 - sy; dy = w - 1 - sx; break;
      default: break;
      }
      const uint8_t *p = rgba + ((size_t)sy * w + sx) * 4u;
      unsigned a = p[3];
      if (a == 0)
        continue;
      /* 0xF81F 是固件的 RGB565 洋红透明色，解成 RGBA 后即 (248,24,248) */
      if (p[0] == 0xF8 && p[1] == 0x18 && p[2] == 0xF8)
        continue;
      int ox = x + dx, oy = y + dy;
      if (!a || a == 0xFF) {
        fb_px(ox, oy, 0xFF000000u | ((unsigned)p[0] << 16) |
                          ((unsigned)p[1] << 8) | p[2]);
        continue;
      }
      uint32_t d = fb_get(ox, oy);
      unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
      unsigned r = (p[0] * a + dr * (255 - a)) / 255;
      unsigned g = (p[1] * a + dg * (255 - a)) / 255;
      unsigned b = (p[2] * a + db * (255 - a)) / 255;
      fb_px(ox, oy, 0xFF000000u | (r << 16) | (g << 8) | b);
    }
  }
}

static void fb_blit_rgba(int x, int y, int w, int h, const uint8_t *rgba) {
  fb_blit_rgba_mode(x, y, w, h, rgba, 0);
}

/* 帧缓冲内区域搬运（IDisplay::BitBlt 用）：把 (sx,sy,w,h) 的像素
 * 整体搬到 (dx,dy)。重叠时用临时缓冲，保证结果与"先拷贝后写入"一致。 */
static void fb_self_blit(int sx, int sy, int w, int h, int dx, int dy) {
  if (!g_fb || w <= 0 || h <= 0)
    return;
  uint32_t *tmp = malloc((size_t)w * (size_t)h * 4u);
  if (!tmp)
    return;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      tmp[(size_t)y * w + x] = fb_get(sx + x, sy + y);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint32_t v = tmp[(size_t)y * w + x];
      if (((v >> 24) & 0xFF) == 0)
        continue; /* 越界处取到 0，不覆盖目标 */
      fb_px(dx + x, dy + y, v | 0xFF000000u);
    }
  free(tmp);
}

/* 把软件帧缓冲上传并呈现 */
/* 把当前帧缓冲存成 PNG（调试用）：ZM_SCREENSHOT=<路径前缀>
 * 会在前若干次 present 时写出 <prefix>N.png，便于核对渲染结果。 */
static void fb_save_png(const char *path) {
  if (!g_fb || g_fb_w <= 0 || g_fb_h <= 0)
    return;
  FILE *fp = fopen(path, "wb");
  if (!fp)
    return;
  png_structp png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = png ? png_create_info_struct(png) : NULL;
  if (!png || !info) {
    if (png)
      png_destroy_write_struct(&png, NULL);
    fclose(fp);
    return;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return;
  }
  png_init_io(png, fp);
  png_set_IHDR(png, info, (png_uint_32)g_fb_w, (png_uint_32)g_fb_h, 8,
               PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  /* 帧缓冲是 ARGB8888（小端内存序 B,G,R,A），PNG 要 R,G,B,A */
  png_bytep row = malloc((size_t)g_fb_w * 4u);
  if (!row) {
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return;
  }
  for (int y = 0; y < g_fb_h; y++) {
    const uint32_t *src = g_fb + (size_t)y * (size_t)g_fb_w;
    for (int x = 0; x < g_fb_w; x++) {
      uint32_t p = src[x];
      row[x * 4 + 0] = (png_byte)((p >> 16) & 0xFF); /* R */
      row[x * 4 + 1] = (png_byte)((p >> 8) & 0xFF);  /* G */
      row[x * 4 + 2] = (png_byte)(p & 0xFF);         /* B */
      row[x * 4 + 3] = 0xFF;
    }
    png_write_row(png, row);
  }
  free(row);
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  fclose(fp);
  log_info("已保存截图: %s", path);
}

static int fb_merge_layer(void);

static void fb_present(void) {
  if (!g_ren || !g_canvas || !g_fb)
    return;
  fb_merge_layer(); /* 先把 applet 层缓冲的内容合并进帧缓冲 */
  SDL_UpdateTexture(g_canvas, NULL, g_fb, g_fb_w * 4);
  SDL_SetRenderTarget(g_ren, NULL);
  SDL_RenderCopy(g_ren, g_canvas, NULL, NULL);
  SDL_RenderPresent(g_ren);

  /* 调试截图：ZM_SCREENSHOT=<前缀> 时保存前 20 帧 */
  static int shot = 0;
  static const char *prefix = NULL;
  static int inited = 0;
  if (!inited) {
    prefix = getenv("ZM_SCREENSHOT");
    inited = 1;
  }
  if (prefix && prefix[0] && shot < 60) {
    /* 统计非黑像素：用来判断"绘制到底有没有落到帧缓冲上" */
    int nonblack = 0;
    for (size_t i = 0, sz = (size_t)g_fb_w * g_fb_h; i < sz; i++)
      if ((g_fb[i] & 0xFFFFFFu) != 0)
        nonblack++;
    char path[512];
    snprintf(path, sizeof(path), "%s%02d.png", prefix, shot);
    fb_save_png(path);
    log_info("present #%d: 非黑像素 %d / %d", shot, nonblack, g_fb_w * g_fb_h);
    shot++;
  }
}

/* 把 applet 的层缓冲（RGB565）合成到宿主帧缓冲。
 * applet 的绘制（BitBlt/sprite 贴图）最终都写在这块客户机可见的缓冲上，
 * 每次 present 前同步一次即可显示。返回写入的像素数（0 表示层还是空的）。 */
static int fb_merge_layer(void) {
  uint32_t *fb = g_fb;
  if (!fb || !g_fb_w || !g_fb_h)
    return 0;
  if (!g_uc)
    return 0; /* unicorn 尚未初始化（zm_display_init 早于 uc_open） */
  uint16_t row[LAYER_W];
  int painted = 0;
  int w = g_fb_w < LAYER_W ? g_fb_w : LAYER_W;
  int h = g_fb_h < LAYER_H ? g_fb_h : LAYER_H;
  for (int y = 0; y < h; y++) {
    if (uc_mem_read(g_uc, LAYER_BUF + (uint32_t)y * LAYER_W * 2u, row,
                    (size_t)w * 2u) != UC_ERR_OK)
      break;
    uint32_t *dst = fb + (size_t)y * (size_t)g_fb_w;
    for (int x = 0; x < w; x++) {
      uint16_t c = row[x];
      if (c == 0)
        continue; /* 未绘制区域（applet 清屏为 0） */
      unsigned r = ((c >> 11) & 0x1F) * 255 / 31;
      unsigned g = ((c >> 5) & 0x3F) * 255 / 63;
      unsigned b = (c & 0x1F) * 255 / 31;
      dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
      painted++;
    }
  }
  return painted;
}

static void fb_commit(void) {
  if (!g_ren)
    return;
  fb_present();

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

int zm_display_init(void) {
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
  /* 画布用 STREAMING 纹理：commit 时从软件帧缓冲整块上传 */
  g_canvas = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_STREAMING, g_w, g_h);
  if (!g_canvas) {
    log_error("SDL_CreateTexture(canvas) failed: %s", SDL_GetError());
    return -1;
  }

  /* 分配软件帧缓冲（applet 的所有绘制都落在它上面） */
  g_fb_w = g_w;
  g_fb_h = g_h;
  g_fb = calloc((size_t)g_fb_w * (size_t)g_fb_h, 4u);
  if (!g_fb) {
    log_error("帧缓冲分配失败");
    return -1;
  }
  fb_clear(0xFF000000u); /* 初始黑屏 */
  fb_present();

  log_info("zm_display_init: 窗口 %dx%d 已创建（软件帧缓冲 %d KB）", g_w, g_h,
           g_fb_w * g_fb_h * 4 / 1024);
  return 0;
}

void zm_display_shutdown(void) {
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

bool zm_display_event_loop(void (*on_click)(uint32_t x, uint32_t y),
                           uint32_t timeout_ms) {
  if (!g_win) {
    log_info("事件循环结束：窗口句柄为空");
    return false;
  }

  /* 先检查是否有待处理的触摸事件（case 10 penUp 跟在 case 9 之后） */
  if (zm_event_dispatch_pending())
    return true; /* 已设置寄存器 → 让模拟器执行 handler */

  /* present 最终画布（init 绘制内容）让用户看到界面 */
  fb_present();

  /* 自动点击（实测用）：ZM_AUTO_CLICK="x,y[,间隔毫秒]" —— 首帧发一次点击，
   * 用于在没有真人操作的环境（脚本/远端）验证触摸链路是否真的通到 applet。
   * 事件码语义已实测确认（00000506 sub_80FC）：
   *   evt=9  PEN_DOWN → obj->vt[0x1C](obj, x, y)
   *   evt=10 PEN_UP   → obj->vt[0x1C](obj, x, y)
   *   evt=11 PEN_MOVE → obj->vt[0x1C](obj, x, y)（拖动）
   * 点击后 handler 通常会重绘界面，可直接用 ZM_SCREENSHOT 对比截图验证。 */
  {
    static int auto_done = 0;
    if (!auto_done && on_click) {
      const char *ac = getenv("ZM_AUTO_CLICK");
      if (ac && ac[0]) {
        auto_done = 1;
        int ax = 0, ay = 0;
        if (sscanf(ac, "%d,%d", &ax, &ay) >= 2) {
          log_info("自动点击测试: (%d, %d)", ax, ay);
          on_click((uint32_t)ax, (uint32_t)ay);
          fb_present();
          return true;
        }
      }
    }
  }

  /* 窗口像素坐标 -> 画布坐标的缩放（窗口尺寸即画布尺寸时为 1:1） */
  int win_w = g_w, win_h = g_h;
  SDL_GetWindowSize(g_win, &win_w, &win_h);

  SDL_Event e;
  Uint32 start = SDL_GetTicks();
  for (;;) {
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        log_info("事件循环结束：收到 SDL_QUIT（窗口关闭）");
        return false; /* 用户关窗 → 模拟结束 */
      }
      if (e.type == SDL_MOUSEBUTTONDOWN && on_click) {
        uint32_t cx = (win_w > 0) ? (uint32_t)(e.button.x * g_w / win_w)
                                  : (uint32_t)e.button.x;
        uint32_t cy = (win_h > 0) ? (uint32_t)(e.button.y * g_h / win_h)
                                  : (uint32_t)e.button.y;
        on_click(cx, cy);
        /* applet 的 touch handler 通常不重绘，但保险起见 present 一次 */
        fb_present();
        return true; /* 已派发点击事件 → 让模拟器执行 handler */
      }
      /* 拖动：按住并移动 → evt=11（PEN_MOVE）。
       * applet 的按下/抬起/移动共用同一个处理函数（见 sub_80FC），
       * 由坐标变化自行判断拖动逻辑（如地图滚动）。
       * 坐标映射与点击一致（窗口尺寸按画布等比缩放）。 */
      if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK)) {
        uint32_t cx = (win_w > 0) ? (uint32_t)(e.motion.x * g_w / win_w)
                                  : (uint32_t)e.motion.x;
        uint32_t cy = (win_h > 0) ? (uint32_t)(e.motion.y * g_h / win_h)
                                  : (uint32_t)e.motion.y;
        on_touch_move(cx, cy);
        return true; /* 让模拟器执行 handler */
      }
    }
    if (timeout_ms != 0 && SDL_GetTicks() - start >= timeout_ms)
      return false; /* 超时 → 模拟结束 */
    if (zm_timer_poll(SDL_GetTicks()))
      return true; /* 定时器回调已挂上跳板 → 让模拟器执行 cb */
    SDL_Delay(16);
  }
}

/* ---------- IDisplay 各槽 ---------- */

/* 通用 stub：记录 offset 与参数，返回 0（不崩） */
uint32_t zm_display_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                         uint32_t r2, uint32_t r3) {
  (void)uc;
  log_debug("display stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1,
            r2, r3);
  return 0;
}

uint32_t zm_display_AddRef(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 1; /* 引用计数：单例返回 1 */
}
uint32_t zm_display_Release(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 0;
}
uint32_t zm_display_GetMaxLayerCount(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 1; /* 单层 */
}
uint32_t zm_display_CreateLayer(uc_engine *uc, uint32_t off, uint32_t r0,
                                uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_CreateLayerExt(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_FreeLayer(uc_engine *uc, uint32_t r0, uint32_t r1) {
  return zm_display_stub(uc, 0x14, r0, r1, 0, 0);
}
uint32_t zm_display_GetLayerInfo(uc_engine *uc, uint32_t off, uint32_t r0,
                                 uint32_t r1, uint32_t r2, uint32_t r3) {
  /* +0x1C GetLayerInfo(display, layer, out)
   * RE（ZMAEE_IDisplay_GetLayerInfo @0x2768C）：无效参数返 -4；
   * layer 表项 +72 标志非 0 才有效，把 +36 起 0x34(52) 字节拷到 out。
   *
   * 00000506 sub_101E8 的用法证明 out+0xC/+0x10 是**宽高**：
   *   sub_19D80(out, 0x34); GetLayerInfo(disp, 1, out);
   *   w = out[0xC]; h = out[0x10];
   *   BitBlt(disp, x, y, surf, {0,0,w,h}, 0, 0);
   * 旧实现写全 0 → 搬运算出的矩形为 0，画面自然什么都看不到。
   * 这里返回屏幕尺寸（模拟器只有一层，尺寸即画布尺寸）。 */
  (void)off;
  (void)r3;
  if (r0 == 0 || r2 == 0)
    return (uint32_t)-4; /* display==0 或 out==null */
  if (r1 != 0 && r1 != 1)
    return (uint32_t)-4; /* 只有 layer 1 */

  uint8_t info[52];
  memset(info, 0, sizeof(info));
  /* +0x0C = 宽，+0x10 = 高 */
  uint32_t w = (uint32_t)LAYER_W;
  uint32_t h = (uint32_t)LAYER_H;
  memcpy(info + 0x0C, &w, 4);
  memcpy(info + 0x10, &h, 4);
  /* +0x24 = **层像素缓冲指针**：applet 会把它存进 BitBlt 的源 surface
   * 的 +0x1C（逆向 sub_10248 @0x10338）。给 0 的话 applet 就拿到空指针，
   * 表现为"绘制调用一大堆、画面全黑"。 */
  uint32_t layer = LAYER_BUF;
  memcpy(info + 0x24, &layer, 4);
  /* 0x34 字节的 layer_info 由调用方栈上提供（sub_101E8 分配 0x34） */
  uc_mem_write(uc, r2, info, sizeof(info));
  return 0;
}
uint32_t zm_display_SetLayerPosition(uc_engine *uc, uint32_t off, uint32_t r0,
                                     uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_Update(uc_engine *uc, uint32_t r0) {
  (void)r0;
  fb_commit(); /* 提交帧缓冲 */
  return 0;
}
uint32_t zm_display_GetActiveLayer(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 0;
}
uint32_t zm_display_UnlockScreen(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 0;
}
uint32_t zm_display_RegisterCustomFont(uc_engine *uc, uint32_t off, uint32_t r0,
                                       uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_GetFontWidth(uc_engine *uc, uint32_t r0, uint32_t r1) {
  return zm_display_stub(uc, 0x44, r0, r1, 0, 0);
}

/* ---- 实测槽 ---- */

/* +0x20：clear(color)，以 color 清屏 */
uint32_t zm_display_clear(uc_engine *uc, uint32_t color) {
  (void)uc;
  fb_clear(color);
  return 0;
}

/* +0x2C：fillRect(rect_ptr,...)。
 * 注意：applet 在绘制末尾调用 fillRect(全屏rect, 1, &0)，语义不明
 * （疑似 invalidate / 带透明度混合，颜色=0 透明）。为避免用黑色覆盖
 * 整张画面，这里保持空实现。 */
uint32_t zm_display_fillRectR(uc_engine *uc, uint32_t rect_ptr) {
  (void)uc;
  (void)rect_ptr;
  return 0;
}

/* +0x40：commit，提交帧缓冲 */
uint32_t zm_display_commit(uc_engine *uc) {
  (void)uc;
  fb_commit();
  return 0;
}

/* +0x48：getWidth，返回屏幕宽度。
 * sub_8062C 用返回值+8 作为文本布局宽度；
 * sub_80790 用返回值+a2 作为文本区域宽度。 */
uint32_t zm_display_getWidth(uc_engine *uc) {
  (void)uc;
  log_info("display[0x48] getWidth -> %u", g_w);
  return g_w;
}

/* +0x4C：measureChar(disp, char_ptr, count, width_out, sp[metrics])
 * sub_802EC 文本布局循环中调用，用于逐字符测量宽度并推进排版游标。
 *   r0=disp, r1=char_ptr(指向 uint16 字符码), r2=count, r3=width_out(int*),
 *   sp[0]=metrics_buf(4B)
 * stub：向 *width_out 写一个固定宽度（约 font_size 的 60%），
 * 避免文本叠在一起。返回 0。 */
uint32_t zm_display_measureChar(uc_engine *uc, uint32_t disp, uint32_t char_ptr,
                                uint32_t count, uint32_t width_out) {
  (void)disp;
  (void)char_ptr;
  (void)count;
  if (width_out)
    uc_write32(uc, width_out, 8);
  return 0;
}

/* +0x50：drawText
 * r1=rect_ptr, r2=text_ptr, r3=text_len, sp=color, sp+8=font_size */
uint32_t zm_display_DrawText(uc_engine *uc, uint32_t rect_ptr, uint32_t text_ptr,
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

/* +0x6C：drawRect，绘制矩形边框
 * r1=x, r2=y, r3=w, sp=h, sp+4=color */
uint32_t zm_display_DrawRect(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                             uint32_t sp) {
  int h = (int)uc_read32(uc, sp);
  uint32_t color = uc_read32(uc, sp + 4);
  fb_draw_rect((int)x, (int)y, (int)w, h, color);
  return 0;
}

/* +0x70：fillRect，填充矩形
 * r1=x, r2=y, r3=w, sp=h, sp+4=color */
uint32_t zm_display_FillRect(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                             uint32_t sp) {
  int h = (int)uc_read32(uc, sp);
  uint32_t color = uc_read32(uc, sp + 4);
  fb_fill_rect((int)x, (int)y, (int)w, h, color);
  return 0;
}

/* ---- 未实测槽（stub） ---- */

uint32_t zm_display_SetTransColor(uc_engine *uc, uint32_t r0, uint32_t r1) {
  return zm_display_stub(uc, 0x54, r0, r1, 0, 0);
}
uint32_t zm_display_SetOpacity(uc_engine *uc, uint32_t off, uint32_t r0,
                               uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_SetClipRect(uc_engine *uc, uint32_t off, uint32_t r0,
                                uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_GetClipRect(uc_engine *uc, uint32_t off, uint32_t r0,
                                uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_SetPixel(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_DrawLine(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_DrawRoundRect(uc_engine *uc, uint32_t off, uint32_t r0,
                                  uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_DrawCircle(uc_engine *uc, uint32_t off, uint32_t r0,
                               uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_FillCircle(uc_engine *uc, uint32_t off, uint32_t r0,
                               uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_DrawArc(uc_engine *uc, uint32_t off, uint32_t r0,
                            uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_FillArc(uc_engine *uc, uint32_t off, uint32_t r0,
                            uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_FillGradientRect(uc_engine *uc, uint32_t off, uint32_t r0,
                                     uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_AlphaBlendRect(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
/* +0x90 DrawImage(this=display, x=r1, y=r2, IImage*=r3[, flags])
 * 00000506 sub_388（资源包装类的绘制分派）type 0 走这里，
 * 传进来的就是 IDisplay::CreateImage 造出的 IImage 对象。 */
uint32_t zm_display_DrawImage(uc_engine *uc, uint32_t off, uint32_t r0,
                              uint32_t r1, uint32_t r2, uint32_t r3) {
  (void)uc;
  (void)off;
  (void)r0;
  int w = 0, h = 0;
  const uint8_t *rgba = NULL;
  if (zm_image_get_pixels(r3, &w, &h, &rgba)) {
    fb_blit_rgba((int)r1, (int)r2, w, h, rgba);
    return 1;
  }
  log_debug("IDisplay.DrawImage: 对象 0x%X 无像素数据（stub）", r3);
  return 0;
}

/* +0x94 DrawBitmap(this=display, x=r1, y=r2, IBitmap*=r3, rect=[sp+0],
 *                  alpha/flag=[sp+4])
 * 00000506 sub_388 type 1 走这里：rect = {left, top, right, bottom}
 * （该路径上固定为 {0,0,w,h}），尺寸由 IBitmap.GetInfo 得到。 */
/* 把某个 surface 对象的指定矩形画到 (dx,dy)：
 * surface 可以是 IImage / IBitmap（都在 zm_image 池里）；
 * rect_ptr = {left, top, right, bottom}（客户机内存），0 表示整图；
 * mode = applet 的类型字节（镜像/翻转，见 fb_blit_rgba_mode）。 */
static int blit_surface_region(uc_engine *uc, uint32_t obj, int dx, int dy,
                               uint32_t rect_ptr, int mode) {
  int w = 0, h = 0;
  const uint8_t *rgba = NULL;
  if (!zm_image_get_pixels(obj, &w, &h, &rgba)) {
    /* 有些对象自身不持像素，而是 +8 处挂着一个 surface（见 sub_27C：
     * 负载 +8 = Decode 结果记录里的 surface）。这里跟随 +8 再试一次。 */
    uint32_t sub_surf = 0;
    if (uc_mem_read(uc, obj + 8, &sub_surf, 4) == UC_ERR_OK && sub_surf &&
        sub_surf != obj && zm_image_get_pixels(sub_surf, &w, &h, &rgba)) {
      obj = sub_surf;
    } else {
      return 0;
    }
  }

  int sx = 0, sy = 0, sw = w, sh = h;
  if (rect_ptr) {
    int left = (int)uc_read32(uc, rect_ptr);
    int top = (int)uc_read32(uc, rect_ptr + 4);
    int right = (int)uc_read32(uc, rect_ptr + 8);
    int bottom = (int)uc_read32(uc, rect_ptr + 12);
    if (right > left && bottom > top) {
      sx = left;
      sy = top;
      sw = right - left;
      sh = bottom - top;
    }
  }
  if (sx == 0 && sy == 0 && sw == w && sh == h) {
    fb_blit_rgba_mode(dx, dy, w, h, rgba, mode);
    return 1;
  }
  if (sx < 0 || sy < 0 || sx + sw > w || sy + sh > h || sw <= 0 || sh <= 0)
    return 0;
  uint8_t *sub = malloc((size_t)sw * sh * 4u);
  if (!sub)
    return 0;
  for (int y = 0; y < sh; y++)
    memcpy(sub + (size_t)y * sw * 4u, rgba + ((size_t)(sy + y) * w + sx) * 4u,
           (size_t)sw * 4u);
  fb_blit_rgba_mode(dx, dy, sw, sh, sub, mode);
  free(sub);
  return 1;
}

uint32_t zm_display_DrawBitmap(uc_engine *uc, uint32_t off, uint32_t r0,
                               uint32_t r1, uint32_t r2, uint32_t r3) {
  (void)off;
  (void)r0;
  /* rect（可选）：{left, top, right, bottom}，用于把源图裁剪后画到 (x,y) */
  if (blit_surface_region(uc, r3, (int)r1, (int)r2, getArg(uc, 4), 0))
    return 1;
  log_debug("IDisplay.DrawBitmap: 对象 0x%X 无像素数据（stub）", r3);
  return 0;
}

/* +0x98 DrawBitmapEx(this=display, x=r1, y=r2, bitmap=r3,
 *                    srcRect=[sp+0], mode=[sp+4], flags=[sp+8])
 *
 * 逆向证据（00000506 sub_42C0 → vt[0x98]）：srcRect={0,0,w,h} 来自记录里的
 * (x1,y1)-(x2,y2)，mode 是 unk_1F714 表选出的"类型字节"（sub_50D8/sub_51A0
 * 会据此对矩形做翻转/转置），flags=0。这是主 sprite 绘制入口。 */
uint32_t zm_display_DrawBitmapEx(uc_engine *uc, uint32_t off, uint32_t r0,
                                 uint32_t r1, uint32_t r2, uint32_t r3) {
  (void)off;
  (void)r0;
  int mode = (int)getArg(uc, 5);
  if (blit_surface_region(uc, r3, (int)r1, (int)r2, getArg(uc, 4), mode))
    return 1;
  log_debug("IDisplay.DrawBitmapEx: 对象 0x%X 无像素数据（stub）", r3);
  return 0;
}
uint32_t zm_display_DrawBitmapFrame(uc_engine *uc, uint32_t off, uint32_t r0,
                                    uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_CreateBitmap(uc_engine *uc, uint32_t r0, uint32_t r1,
                                 uint32_t r2, uint32_t r3) {
  (void)uc;
  (void)r0;
  (void)r1;
  (void)r2;
  (void)r3;
  log_info("IDisplay.CreateBitmap -> BITMAP (stub 单例)");
  return BITMAP; /* 返回 bitmap 单例对象 */
}
uint32_t zm_display_LoadBitmap(uc_engine *uc, uint32_t r0, uint32_t r1) {
  (void)uc;
  (void)r0;
  (void)r1;
  log_info("IDisplay.LoadBitmap -> BITMAP (stub 单例)");
  return BITMAP; /* 返回 bitmap 单例对象 */
}
/* +0xA8 CreateImage(this=display, alloc=r1, free=r2, out=&IImage=r3)
 * applet 资源加载链的第一步（00000506 sub_313C）：造一个 IImage，
 * 随后 SetData(文件名) → Decode(出 IBitmap)。旧的 stub 不写 out，
 * applet 会把栈上的脏值当对象指针用 → 立即段错误。 */
uint32_t zm_display_CreateImage(uc_engine *uc, uint32_t off, uint32_t r0,
                                uint32_t r1, uint32_t r2, uint32_t r3) {
  (void)off;
  return zm_image_CreateImage(uc, r0, r1, r2, r3);
}
/* +0xAC BitBlt(this=display, x=r1, y=r2, surface=r3, rect=[sp+0],
 *              mode=[sp+4], flags=[sp+8])
 *
 * 逆向（ZMAEE_IDisplay_BitBlt）：
 *   if (a4 != 0 && result != 0 && a6 <= 7 && a5 != 0)
 *       ZMAEE_GDI_BitBlt_Ext(layer + 36, a2=x, a3=y, a4=surface,
 *                            a5=rect, a6=mode);
 *   → 参数是 (display, x, y, surface, rect, mode) 共 6 个；
 *     mode 取值 0..7，作为 byte_5B658[mode+8] 的索引选择 GDI 搬运函数。
 *   → surface 是一个带像素描述的对象（surface+8=纹理指针、+12=格式/标志），
 *     即资源包装里的图片对象；rect={left,top,right,bottom}。
 * 实测（00000506 sub_101E8 绘制英雄格）：
 *   BitBlt(disp, 42, 215, hero, {0,0,px,py}, 0/1/3/6, 0)
 *   其中 mode 由英雄朝向（direction）决定 —— 正是镜像/翻转编码。
 * 因此这里把 surface 的指定矩形按 mode 翻转后贴到 (x,y)。
 * 兼容：surface 为 0（或拿不到像素）时保留旧的"帧缓冲区域搬运"行为。 */
uint32_t zm_display_BitBlt(uc_engine *uc, uint32_t off, uint32_t r0,
                           uint32_t r1, uint32_t r2, uint32_t r3) {
  (void)off;
  (void)r0;
  uint32_t rect = getArg(uc, 4);
  int mode = (int)getArg(uc, 5);
  int flags = (int)getArg(uc, 6);

  /* 路径 1：a4 是 applet 自造的 ZMAEE_GDI_Surface（实测就是这条路）
   *   结构 {宽, 高, 位深(1/2/3/4), 透明色}，像素紧跟结构之后 */
  if (r3 && zm_image_blit_gdi_surface(uc, r3, (int)r1, (int)r2, rect, mode))
    return 1;

  /* 路径 2：a4 是模拟器图像池里的对象（entry / surface / IBitmap） */
  if (r3 && blit_surface_region(uc, r3, (int)r1, (int)r2, rect, mode))
    return 1;

  /* 回退路径：从帧缓冲里搬一块区域（滚动/合成）。
   * mode: 0/1 = 直接搬，2 = 窗口内容下滚一行（补 null 行），
   *       3..7 = 方向翻转。flags 位 0/1 映射闪烁亮度，这里忽略。 */
  (void)flags;
  if (!g_fb)
    return 0;
  int sx = 0, sy = 0, sw = g_fb_w, sh = g_fb_h;
  if (rect) {
    int l = (int)uc_read32(uc, rect);
    int t = (int)uc_read32(uc, rect + 4);
    int rr = (int)uc_read32(uc, rect + 8);
    int b = (int)uc_read32(uc, rect + 12);    if (rr > l && b > t) {
      sx = l;
      sy = t;
      sw = rr - l;
      sh = b - t;
    }
  }
  if (mode == 0 || mode == 1) {
    fb_self_blit(sx, sy, sw, sh, (int)r1, (int)r2);
    return 1;
  }
  return 0;
}
uint32_t zm_display_Flatten(uc_engine *uc, uint32_t off, uint32_t r0,
                            uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_StretchBlt(uc_engine *uc, uint32_t off, uint32_t r0,
                               uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_DrawAntialiasingLine(uc_engine *uc, uint32_t off, uint32_t r0,
                                         uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_DrawWLine(uc_engine *uc, uint32_t off, uint32_t r0,
                              uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_GetDMLayerHdlr(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_RelevanceLayer(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_Refresh(uc_engine *uc) {
  (void)uc;
  fb_commit(); /* 提交帧缓冲 */
  return 0;
}
uint32_t zm_display_DrawImageExt(uc_engine *uc, uint32_t off, uint32_t r0,
                                 uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_DrawSysWallPaper(uc_engine *uc, uint32_t off, uint32_t r0,
                                     uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_DrawBorderText(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_PushAndSetAlphaLayer(uc_engine *uc, uint32_t off, uint32_t r0,
                                         uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_PopAndRestoreAlphaLayer(uc_engine *uc, uint32_t off,
                                            uint32_t r0, uint32_t r1,
                                            uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_display_RotateScreen(uc_engine *uc, uint32_t off, uint32_t r0,
                                 uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}

/* ---------- IBitmap 各槽 ---------- */

uint32_t zm_bitmap_AddRef(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 1;
}
uint32_t zm_bitmap_Release(uc_engine *uc, uint32_t r0) {
  (void)uc;
  /* 解码出的 IBitmap 是对象池句柄，释放走图像池 */
  return zm_image_Release(uc, r0);
}
uint32_t zm_bitmap_SetTransColor(uc_engine *uc, uint32_t r0, uint32_t r1) {
  (void)uc;
  (void)r0;
  (void)r1;
  return 0;
}
uint32_t zm_bitmap_sub_25F78(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_bitmap_GetInfo(uc_engine *uc, uint32_t r0, uint32_t r1) {
  /* IBitmap.GetInfo(info_ptr)：填 {width, height, ...}。
   * 00000506 sub_388 type 1 的用法确认：
   *   bitmap->vt[16](bitmap, v10) → v13=v10[0], v14=v10[1] 随即被当 rect 的
   *   right/bottom（即宽高）用，故此处必须写 **dword** 宽高。 */
  int w = 0, h = 0;
  const uint8_t *rgba = NULL;
  if (zm_image_get_pixels(r0, &w, &h, &rgba)) {
    if (r1) {
      uc_write32(uc, r1, (uint32_t)w);
      uc_write32(uc, r1 + 4, (uint32_t)h);
    }
    return 1;
  }
  if (r1) {
    uint8_t zero[16] = {0};
    uc_mem_write(uc, r1, zero, sizeof(zero));
  }
  return 1;
}
uint32_t zm_bitmap_sub_25F84(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}
uint32_t zm_bitmap_sub_25FF8(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3) {
  return zm_display_stub(uc, off, r0, r1, r2, r3);
}

#include "zm_display.h"

#include "../../emu.h"
#include "../../event.h"
#include "../../log/log.h"
#include "../../trap.h" /* getArg：第 5 个及以后的参数 */
#include "../../tool/uc_helper.h"
#include "../runtime/timer/zm_timer.h" /* zm_timer_poll：到期定时器检查 */
#include "zm_image.h" /* IImage/IBitmap 像素取用（DrawImage/DrawBitmap） */
#include "zm_layer.h" /* IDisplay 层结构（CreateLayer/GetLayerInfo/合成） */
#include "../fs/zm_file_mgr.h" /* zm_fs_read_file：宿主侧整文件读取 */

#include <stdbool.h>
#include <stdlib.h>
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
/* RGB565==0 是否当透明跳过。
 * **默认关闭**：黑色是游戏里大量使用的正常颜色（实测占 15~18%），
 * 把它当透明跳过等于这些像素永远不更新 → 精灵移动后旧位置变黑也不刷新，
 * 保留着上一帧的旧精灵 → **残影**（实测帧缓冲黑像素从 119438 累积到 140220）。
 * 这是早期"层还没实现"时用来透出背景的权宜做法，现已无必要。
 * ZM_FB_MASK0=1 可临时恢复旧行为做对照实验。 */
static bool g_fb_mask0 = false;
static bool g_fb_mask0_inited = false;

/* 当前层透明色（color key）。
 *
 * ZMAEE 的透明是「透明色」而非 alpha 通道：
 *   - ZMAEE_IDisplay_SetTransColor(this, c2, c3) → 层结构 +80 = c2、+84 = c3
 *   - ZMAEE_IBitmap_SetTransColor(bmp, c)        → 位图对象 +20 = c
 * 实测 00000506 调的是 SetTransColor(DISPLAY, 0x1)，即透明色 = RGB565 的
 * 0x0001（近黑，刻意避开真正的黑 0x0000）。
 *
 * 旧实现把透明色硬编码成 0，恰好和游戏设的 1 相反：真正的黑被当透明丢掉
 * （透出上一帧 → 残影），真正的透明像素却被画了出来。
 * ZM_TRANS_KEY=<hex> 可强制指定，便于对照实验。 */
static uint32_t g_trans_color = 0;
static uint32_t g_trans_color2 = 0; /* 第二个值（掩码/替换色），语义待定 */
static bool g_trans_forced = false;
/* ZM_COLORSTAT=1：统计层缓冲 RGB565 颜色直方图（诊断透明色是否被写入） */
static bool g_colorstat = false;
/* 本次统计窗口内被"跳过"（保留上一帧像素）的计数，用于诊断拖影 */
static uint32_t g_skipped = 0;

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

/* 把"层缓冲"（RGB565）原样存成 PNG，用来直接观察 applet 到底往层里画了什么。
 * 透明色（品红）会原样保留 —— 品红面积就是"层没有覆盖"的部分。
 * 仅调试用：ZM_DUMP_LAYER=<前缀>。 */
static void layer_dump_png(uc_engine *uc, const char *path, const zm_layer_t *L) {
  if (!uc || !path || !L || !L->w || !L->h || !L->buf)
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
  png_set_IHDR(png, info, (png_uint_32)L->w, (png_uint_32)L->h, 8,
               PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  uint16_t *srow = malloc((size_t)L->w * 2u);
  png_bytep row = malloc((size_t)L->w * 4u);
  if (!srow || !row) {
    free(srow);
    free(row);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return;
  }
  for (uint32_t y = 0; y < L->h; y++) {
    if (uc_mem_read(uc, L->buf + y * L->w * 2u, srow, (size_t)L->w * 2u) !=
        UC_ERR_OK)
      break;
    for (uint32_t x = 0; x < L->w; x++) {
      uint16_t c = srow[x];
      row[x * 4 + 0] = (png_byte)(((c >> 11) & 0x1F) * 255 / 31); /* R */
      row[x * 4 + 1] = (png_byte)(((c >> 5) & 0x3F) * 255 / 63);  /* G */
      row[x * 4 + 2] = (png_byte)((c & 0x1F) * 255 / 31);         /* B */
      row[x * 4 + 3] = 0xFF;
    }
    png_write_row(png, row);
  }
  free(srow);
  free(row);
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  fclose(fp);
  log_info("已保存层缓冲图: %s", path);
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

  /* 调试截图：ZM_SCREENSHOT=<前缀> 时保存帧图。
   *   ZM_SHOT_EVERY=<N> 每 N 个 present 存一张（默认 1）
   *   ZM_SHOT_MAX=<N>   最多存 N 张（默认 60）
   * 配合 ZM_SHOT_EVERY 可以跨帧采样，用来观察动画（如精灵拖影）。 */
  static int shot = 0;
  static uint32_t present_no = 0;
  static const char *prefix = NULL;
  static const char *shot_every_s = NULL;
  static const char *shot_max_s = NULL;
  static int inited = 0;
  present_no++;
  if (!inited) {
    prefix = getenv("ZM_SCREENSHOT");
    shot_every_s = getenv("ZM_SHOT_EVERY");
    shot_max_s = getenv("ZM_SHOT_MAX");
    inited = 1;
  }
  int every = (shot_every_s && shot_every_s[0]) ? atoi(shot_every_s) : 1;
  int smax = (shot_max_s && shot_max_s[0]) ? atoi(shot_max_s) : 60;
  if (every < 1)
    every = 1;
  if (prefix && prefix[0] && shot < smax && (present_no % (uint32_t)every) == 0) {
    /* 统计非黑像素：用来判断"绘制到底有没有落到帧缓冲上" */
    int nonblack = 0;
    for (size_t i = 0, sz = (size_t)g_fb_w * g_fb_h; i < sz; i++)
      if ((g_fb[i] & 0xFFFFFFu) != 0)
        nonblack++;
    char path[512];
    snprintf(path, sizeof(path), "%s%02d.png", prefix, shot);
    fb_save_png(path);
    log_info("present #%u (第%d张, 每%d帧): 非黑像素 %d / %d", present_no, shot,
             every, nonblack, g_fb_w * g_fb_h);
    shot++;
  }
}

/* 把 applet 的层缓冲（RGB565）合成到宿主帧缓冲。
 * applet 的绘制（BitBlt/sprite 贴图）最终都写在这块客户机可见的缓冲上，
 * 每次 present 前同步一次即可显示。返回写入的像素数（0 表示层还是空的）。 */
/* 合成时当作"透明"跳过的 RGB565 键。
 * 默认 0（与历史行为一致）；ZM_TRANS_KEY=<hex> 可覆盖做对照实验。
 * 注意：这里刻意**不**跟随 SetTransColor 的值 —— 实测把游戏设的 0x1
 * 用作合成键会改变既有画面，而层缓冲里 0x0000/0x0001 都几乎不出现，
 * 收益为零。SetTransColor 的值仍会记录并写进层结构的 +0x2C/+0x30，
 * 供 applet 自己读取。 */
static uint32_t g_merge_key = 0;

/* 诊断开关 ZM_NO_SKIP=1：合成时**完全不做透明跳过**（层整行直写）。
 * 用来一刀切开"残影来自哪里"：
 *   - 若残影消失 → 残影是"帧缓冲保留了上一帧的旧值"造成的
 *   - 若层没覆盖到的区域变成品红块 → 说明 applet 并不每帧重画背景，
 *     背景其实是靠帧缓冲留存下来的
 * 注意它同时让 fb_composite_layers 和 fb_merge_layer 都不跳过，
 * 否则品红会被写进帧缓冲再被 merge 跳过，诊断失效。 */
static bool g_no_skip = false;
static bool g_no_skip_inited = false;

/* 把 applet 的层缓冲（RGB565）合成到宿主帧缓冲。
 * applet 的绘制（自带 GDI 的 BitBlt/mask blit）最终都写在这块客户机可见
 * 的缓冲上，每次 present 前同步一次即可显示。返回写入的像素数。 */
/* 层 → 帧缓冲：等价于固件 IDisplay_Update / UpdateEx 的合成循环。
 * RE：Update(display,x,y,w,h) → UpdateEx(display,{x,y,w,h},4,{0,1,2,3})
 *     UpdateEx: LockFrameBuffer(&desc) → desc[5..8]=裁剪矩形
 *               → 逐层 sub_285D8(desc, 层载荷)   （内部走 ZMAEE_Blt）
 *               → UnLockFrameBuffer(desc)        （内部 AndroidAEE_Update 上屏）
 * 我们等价拆成两步：先合成到 guest 帧缓冲，再由宿主转 g_fb 上屏。
 * 帧缓冲描述符（LockFrameBuffer 填的）：
 *   [0]色深(=1 → RGB565) [1]x=0 [2]y=0 [3]宽(pitch) [4]高
 *   [5..8]裁剪矩形（UpdateEx 后填） [9]基址 */
static void fb_composite_layers(int rw, int rh) {
  if (!g_uc || rw <= 0 || rh <= 0)
    return;
  static uint16_t row[LAYER_W];
  static uint16_t frow[LAYER_W];
  /* 叠加顺序。
   *
   * 【实测决定】对比"建层 0 / 不建层 0"两种行为：
   *   不建层 0（SetActiveLayer(0) 恒失败）→ applet 退回"全画进层 1"
   *                                         → 鱼可见，但层不清 → 残影
   *   建层 0（SetActiveLayer(0) 成功）    → applet 走正规路径：
   *                                         背景进层 1、**鱼进层 0**，
   *                                         而层 1 是 100% 不透明的
   * 所以层 0 必须**最后**叠加，否则被层 1 盖住。
   * 对照开关：ZM_LAYER_ORDER=0 恢复"层 0 先叠加"的旧顺序。 */
  static int layer0_last = -1;
  if (layer0_last < 0) {
    const char *e = getenv("ZM_LAYER_ORDER");
    layer0_last = (e && e[0] == '0') ? 0 : 1;
    log_info("[合成] 层 0 %s叠加", layer0_last ? "最后" : "最先");
  }
  for (uint32_t step = 0; step < ZM_LAYER_COMPOSITE_MAX; step++) {
    uint32_t li;
    if (layer0_last)
      li = (step + 1u) % ZM_LAYER_COMPOSITE_MAX; /* 1,2,...,15,0 */
    else
      li = step;
    zm_layer_t L;
    if (zm_layer_get(g_uc, DISPLAY, li, &L) != 0)
      continue; /* 该层不存在 */
    if (L.fmt != 1)
      continue; /* 只合成 16bit(RGB565) 层 */
    int pitch = (int)L.w;
    int lay_h = (int)L.h;
    if (pitch <= 0 || lay_h <= 0)
      continue;
    /* RE：ZMAEE_Blt 里目标位置 = 层.x - 帧缓冲.x，我们帧缓冲原点恒为 0 */
    int dx = (int)L.x;
    int dy = (int)L.y;
    if (dx < 0)
      dx = 0;
    if (dy < 0)
      dy = 0;
    if (dx >= rw || dy >= rh)
      continue;
    int lw = (pitch < rw - dx) ? pitch : rw - dx;
    int lh = (lay_h < rh - dy) ? lay_h : rh - dy;
    if (lw <= 0 || lh <= 0)
      continue;
    /* 本层透明 key：RE：+0x2C 非 0 才启用，颜色在 +0x30（转 RGB565）
     *
     * 【实测补充】层 0（基础层）的 +0x2C 实测为 0，但 applet 明确用它那套
     * 全局透明色（品红 0xF81F）来"清空"层 0 —— 实测层 0 有 100% 是品红。
     * 若按"未启用"整行直写，层 0 会把整屏刷成品红。
     * 所以这里回退到 applet 通过 SetTransColor 设过的全局透明色。 */
    uint16_t tc;
    int use_tc;
    if (L.tenable != 0) {
      tc = to_rgb565(L.tcolor);
      use_tc = 1;
    } else if (g_trans_color2) {
      tc = to_rgb565(g_trans_color2); /* 回退：全局透明色（品红） */
      use_tc = 1;
    } else {
      tc = 0xFFFFu;
      use_tc = 0;
    }
    if (g_no_skip)
      use_tc = 0; /* 诊断：整行直写，不做透明跳过 */
    for (int y = 0; y < lh; y++) {
      if (uc_mem_read(g_uc, L.buf + (uint32_t)y * (uint32_t)pitch * 2u, row,
                      (size_t)lw * 2u) != UC_ERR_OK)
        break;
      uint32_t fb_off = (uint32_t)(y + dy) * (uint32_t)LAYER_W * 2u +
                        (uint32_t)dx * 2u;
      if (uc_mem_read(g_uc, FRAMEBUF + fb_off, frow, (size_t)lw * 2u) !=
          UC_ERR_OK)
        break;
      if (use_tc) {
        for (int x = 0; x < lw; x++)
          if (row[x] != tc)
            frow[x] = row[x]; /* 透明处保留帧缓冲原有内容（= 透出下层） */
      } else {
        for (int x = 0; x < lw; x++)
          frow[x] = row[x];
      }
      uc_mem_write(g_uc, FRAMEBUF + fb_off, frow, (size_t)lw * 2u);
    }
  }
}

/* 把 guest 帧缓冲（RGB565）转成宿主帧缓冲显示。
 * 返回写入的像素数（0 表示还没内容）。 */
static int fb_merge_layer(void) {
  uint32_t *fb = g_fb;
  if (!fb || !g_fb_w || !g_fb_h)
    return 0;
  if (!g_uc)
    return 0; /* unicorn 尚未初始化（zm_display_init 早于 uc_open） */
  static uint32_t hist[65536];
  static uint32_t mhist[65536]; /* 品红家族（透明 key 色域）专用直方图 */
  static int frames = 0;
  static bool stat_inited = false;
  static int probe_on = 2;
  if (!stat_inited) {
    stat_inited = true;
    g_colorstat = getenv("ZM_COLORSTAT") && getenv("ZM_COLORSTAT")[0] == '1';
    probe_on = (getenv("ZM_PROBE") && getenv("ZM_PROBE")[0] == '1') ? 1 : 0;
    const char *e = getenv("ZM_TRANS_KEY");
    if (e && e[0])
      g_merge_key = (uint32_t)strtoul(e, NULL, 16);
    /* 终于把这个开关接上（以前只有声明、从没读环境变量，等于关不掉） */
    {
      const char *m = getenv("ZM_FB_MASK0");
      g_fb_mask0 = (m && m[0] == '1');
      g_fb_mask0_inited = true;
      if (g_fb_mask0)
        log_info("[诊断] ZM_FB_MASK0=1：恢复「黑色当透明跳过」的旧行为");
    }
    memset(hist, 0, sizeof(hist));
    memset(mhist, 0, sizeof(mhist));
  }
  int nmag = 0;
  int w = g_fb_w < LAYER_W ? g_fb_w : LAYER_W;
  int h = g_fb_h < LAYER_H ? g_fb_h : LAYER_H;

  /* 观测：applet 有没有自己往客户机帧缓冲里写？
   * 我们在上次合成后把帧缓冲整块存下来，这次合成**之前**再比对一遍；
   * 差异像素就是 applet 自己（绕过 trap）写进去的。
   * 若这个数接近 0，说明背景完全靠我们合成的结果留存 —— 也就解释了
   * 为什么精灵移走后旧位置擦不掉。 */
  {
    static uint16_t prev_fb[LAYER_W * LAYER_H];
    static int have_prev = 0;
    static int applet_wrote = 0;
    if (probe_on) {
      if (have_prev) {
        uint16_t cur[LAYER_W];
        applet_wrote = 0;
        for (int y = 0; y < h; y++) {
          if (uc_mem_read(g_uc, FRAMEBUF + (uint32_t)y * LAYER_W * 2u, cur,
                          (size_t)w * 2u) != UC_ERR_OK)
            break;
          for (int x = 0; x < w; x++)
            if (cur[x] != prev_fb[(size_t)y * LAYER_W + x])
              applet_wrote++;
        }
      }
    }

    /* 宿主驱动的那一步：层 → 帧缓冲（固件里由 IDisplay_Update 完成） */
    fb_composite_layers(w, h);

    if (probe_on) {
      for (int y = 0; y < h; y++)
        uc_mem_read(g_uc, FRAMEBUF + (uint32_t)y * LAYER_W * 2u,
                    &prev_fb[(size_t)y * LAYER_W], (size_t)w * 2u);
      have_prev = 1;
      if ((frames % 60) == 0)
        log_info("  [探针]applet 自行写入帧缓冲的像素: %d/帧（0=背景完全靠合成留存）",
                 applet_wrote);
    }
  }

  uint16_t tc = g_trans_color2 ? to_rgb565(g_trans_color2) : 0;
  int use_tc = (g_trans_color2 != 0);
  if (!g_no_skip_inited) {
    g_no_skip_inited = true;
    const char *e = getenv("ZM_NO_SKIP");
    g_no_skip = (e && e[0] == '1');
    if (g_no_skip)
      log_info("[诊断] ZM_NO_SKIP=1：合成不做透明跳过（层整行直写）");
  }
  if (g_no_skip)
    use_tc = 0;

  uint16_t row[LAYER_W];
  int painted = 0;
  for (int y = 0; y < h; y++) {
    if (uc_mem_read(g_uc, FRAMEBUF + (uint32_t)y * LAYER_W * 2u, row,
                    (size_t)w * 2u) != UC_ERR_OK)
      break;
    uint32_t *dst = fb + (size_t)y * (size_t)g_fb_w;
    for (int x = 0; x < w; x++) {
      uint16_t c = row[x];
      if (g_colorstat)
        hist[c]++;
      if (probe_on) {
        /* 品红家族：R 高、G 低、B 高。透明 key(0xF81F) 就在这个色域里，
         * 若这里出现大量"接近但不等于"key 的值，说明 applet 画的透明底
         * 并非精确 0xF81F，我们的精确匹配会漏掉 → 品红条残留。 */
        if (((c >> 11) & 0x1F) > 0x10 && ((c >> 5) & 0x3F) < 0x08 &&
            (c & 0x1F) > 0x10) {
          nmag++;
          mhist[c]++;
        }
      }
      if (use_tc && c == tc) {
        /* 帧缓冲里仍是透明色 → 该处没有任何层覆盖，保留上一帧 */
        g_skipped++;
        continue;
      }
      if (g_fb_mask0 && c == g_merge_key) {
        g_skipped++;
        continue;
      }
      unsigned r = ((c >> 11) & 0x1F) * 255 / 31;
      unsigned g = ((c >> 5) & 0x3F) * 255 / 63;
      unsigned b = (c & 0x1F) * 255 / 31;
      dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
      painted++;
    }
  }
  /* 观测探针：ZM_PROBE=1 时每 60 帧汇总一次 */
  frames++;
  if (probe_on && (frames % 60) == 0) {
    /* 全帧找"近黑"像素的包围盒：黑底若真在 g_fb 里，这里必然现形 */
    int bx0 = w, bx1 = -1, by0 = h, by1 = -1, nblack = 0, ngray = 0;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        uint32_t c = fb[(size_t)y * g_fb_w + x] & 0xFFFFFFu;
        int r = (int)((c >> 16) & 0xFF), gg = (int)((c >> 8) & 0xFF),
            b = (int)(c & 0xFF);
        if (r < 24 && gg < 24 && b < 24) {
          nblack++;
          if (x < bx0) bx0 = x;
          if (x > bx1) bx1 = x;
          if (y < by0) by0 = y;
          if (y > by1) by1 = y;
        } else if (r < 64 && gg < 64 && b < 64) {
          ngray++;
        }
      }
    }
    log_info("  [探针]近黑像素 %d bbox=[%d,%d]-[%d,%d]  暗灰 %d", nblack, bx0,
             by0, bx1, by1, ngray);
    /* 品红家族 top-3：确认 applet 画的透明底是不是精确 0xF81F */
    if (nmag > 0) {
      for (int k = 0; k < 3; k++) {
        uint32_t best = 0, bestc = 0;
        for (unsigned i = 0; i < 65536u; i++)
          if (mhist[i] > best) {
            best = mhist[i];
            bestc = i;
          }
        if (best == 0)
          break;
        log_info("  [探针]品红家族 #%d: 0x%04X × %u (命中 key 0x%04X ? %s)", k + 1,
                 bestc, best, tc, (bestc == tc) ? "是" : "否 ← 漏掉");
        mhist[bestc] = 0;
      }
    }
    log_info("  [探针]品红家族像素 %d/帧（其中精确等于 key 的会被跳过）", nmag / 60);
    /* 基础层（applet 用 GetBaseLayerBuffer 拿到的缓冲）里有没有内容？
     * 有 → applet 确实把背景画在基础层；空 → 它没用这个缓冲。 */
    {
      uint32_t ba = zm_display_base_layer_addr();
      if (ba) {
        static uint16_t brow[LAYER_W];
        uint32_t nz = 0, total = 0;
        for (int y = 0; y < LAYER_H; y++) {
          if (uc_mem_read(g_uc, ba + (uint32_t)y * LAYER_W * 2u, brow,
                          (uint32_t)LAYER_W * 2u) != UC_ERR_OK)
            break;
          for (int x = 0; x < LAYER_W; x++) {
            total++;
            if (brow[x])
              nz++;
          }
        }
        log_info("  [探针]基础层 0x%X: 非零 %u/%u (%.1f%%)", ba, nz, total,
                 total ? 100.0 * nz / total : 0.0);
      }
    }
    /* 列出所有存在的层：判断"背景是不是在另一层里"（若层 0 存在＝背景层） */
    {
      static int once = 1;
      if (once) {
        once = 0;
        int exist = 0;
        for (uint32_t li = 0; li < 16; li++) {
          zm_layer_t L2;
          if (zm_layer_get(g_uc, DISPLAY, li, &L2) != 0)
            continue;
          exist++;
          log_info("  [探针]层%u 存在: %ux%d fmt=%u @(%d,%d) buf=0x%X "
                   "tenable=%u tcolor=0x%08X",
                   li, L2.w, L2.h, L2.fmt, (int)L2.x, (int)L2.y, L2.buf,
                   L2.tenable, L2.tcolor);
        }
        log_info("  [探针]共 %d 个层存在；活动层=%u", exist,
                 uc_read32(g_uc, DISPLAY + 8));
      }
    }
    /* 逐层统计：每个存在的层里"透明色"与"实际内容"的占比。
     * 这是判断"谁画了什么"最直接的依据：
     *   层 0 若几乎全透明 → applet 清了它却没画进去（绘制被 stub 丢了） */
    {
      static uint16_t lrow2[512];
      static uint32_t lh2[65536];
      for (uint32_t li = 0; li < ZM_LAYER_COMPOSITE_MAX; li++) {
        zm_layer_t L3;
        if (zm_layer_get(g_uc, DISPLAY, li, &L3) != 0 || !L3.w || !L3.h)
          continue;
        if (L3.w > 512)
          continue;
        uint16_t tcx = to_rgb565(L3.tcolor);
        uint32_t total = 0, ntrans = 0;
        memset(lh2, 0, sizeof(lh2));
        for (uint32_t yy = 0; yy < L3.h; yy++) {
          if (uc_mem_read(g_uc, L3.buf + yy * L3.w * 2u, lrow2,
                          (size_t)L3.w * 2u) != UC_ERR_OK)
            break;
          for (uint32_t xx = 0; xx < L3.w; xx++) {
            total++;
            lh2[lrow2[xx]]++;
            if (lrow2[xx] == tcx)
              ntrans++;
          }
        }
        log_info("  [探针]层%u: 共 %u, 透明(key=0x%04X %s) %u (%.1f%%), "
                 "内容 %u (%.1f%%)",
                 li, total, tcx, L3.tenable ? "启用" : "未启用", ntrans,
                 total ? 100.0 * ntrans / total : 0.0, total - ntrans,
                 total ? 100.0 * (total - ntrans) / total : 0.0);
        for (int k = 0; k < 4; k++) {
          uint32_t best = 0, bestc = 0;
          for (unsigned i = 0; i < 65536u; i++)
            if (lh2[i] > best) {
              best = lh2[i];
              bestc = i;
            }
          if (best == 0)
            break;
          log_info("    层%u 内 #%d: 0x%04X × %u (%.1f%%)", li, k + 1, bestc, best,
                   total ? 100.0 * best / total : 0.0);
          lh2[bestc] = 0;
        }
        /* ZM_DUMP_LAYER=<前缀>：每一层的缓冲都存成图，
         * 直接看 applet 到底把什么画进了哪个层。 */
        {
          static const char *dump_prefix = NULL;
          static int dump_inited = 0;
          if (!dump_inited) {
            dump_prefix = getenv("ZM_DUMP_LAYER");
            dump_inited = 1;
          }
          if (dump_prefix && dump_prefix[0]) {
            char lp[512];
            snprintf(lp, sizeof(lp), "%s_l%u_%u.png", dump_prefix, li,
                     (uint32_t)(frames / 60));
            layer_dump_png(g_uc, lp, &L3);
          }
        }
      }
    }
    nmag = 0;
    memset(mhist, 0, sizeof(mhist));
  }
  if (g_colorstat && (frames % 60) == 0) {
    log_info("  帧缓冲: 0x0000=%u 0x0001=%u 0xF81F=%u 0xFFFF=%u 总计=%u",
             hist[0x0000], hist[0x0001], hist[0xF81F], hist[0xFFFF],
             (uint32_t)((size_t)w * h * 60));
    log_info("  跳过(保留上一帧) %u 像素/帧", g_skipped / 60);
    g_skipped = 0;
    for (int k = 0; k < 6; k++) {
      uint32_t best = 0, bestc = 0;
      for (unsigned i = 0; i < 65536u; i++)
        if (hist[i] > best) {
          best = hist[i];
          bestc = i;
        }
      if (best == 0)
        break;
      log_info("  颜色 #%d: 0x%04X × %u", k + 1, bestc, best);
      hist[bestc] = 0;
    }
    memset(hist, 0, sizeof(hist));
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

void zm_display_size(int *w, int *h) {
  if (w)
    *w = g_fb_w;
  if (h)
    *h = g_fb_h;
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

    /* 每轮呈现一次帧缓冲。
     *
     * 必须做：00000506 这类 applet 自带 GDI（ZMAEE_GDI_BitBlt_Ext 等），
     * 定时器回调里**直接写层像素缓冲**，不走 IDisplay 的绘制 trap，也
     * 几乎不调 Update/commit（实测 20 秒内 IDisplay 只有 LoadBitmap 有
     * 调用量）。因此若不在这里主动 present，画面永远停在初始黑屏。
     * 本轮开始时上一轮的回调已经跑完，缓冲里就是刚画好的一帧。 */
    fb_present();
    SDL_Delay(16);
  }
}

/* ---------- IDisplay 各槽 ---------- */

/* 通用 stub：记录 offset 与参数，返回 0（不崩） */
/* ---- IDisplay 虚表槽位调用计数 ----
 * 目的：一次运行就能看清"哪些槽真的被 applet 调用、哪些从未被调用"，
 * 避免把没被调用的槽位错位误判成问题。
 * 每累计 600 次调用汇总打印一次（INFO 级，配合 ZM_LOG=info 观察）。 */
static uint32_t g_slot_calls[64];
static uint32_t g_slot_total = 0;
static void zm_display_slot_tick(uint32_t off) {
  uint32_t i = off / 4u;
  if (i < 64u)
    g_slot_calls[i]++;
  if ((++g_slot_total % 600u) != 0u)
    return;
  log_info("[槽位统计] 累计 %u 次调用：", g_slot_total);
  for (uint32_t k = 0; k < 64u; k++)
    if (g_slot_calls[k])
      log_info("   +0x%02X : %u 次", k * 4u, g_slot_calls[k]);
}

uint32_t zm_display_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                         uint32_t r2, uint32_t r3) {
  (void)uc;
  zm_display_slot_tick(off);
  log_debug("display stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1,
            r2, r3);
  return 0;
}

/* IDisplay 虚表 +0x18：RE 为 `ZMAEE_IDisplay_GetBaseLayerBuffer()`
 *   → 无参，返回全局的基础层缓冲指针 unk_64B60（配套还有
 *     `ZMAEE_IDisplay_GetBaseLayerDepth()` → unk_64B94，默认深度 1=RGB565）。
 *
 * 【为什么这个很重要】
 * 层数组里 **层 0 是"基础层"**（RE 的 `ZMAEE_IDisplay_FreeAllLayer` 从 i=1
 * 开始循环、永远不释放层 0 就是铁证）。背景应当画在基础层里常驻，精灵画在
 * 层 1..15、每帧清空重画 —— 合成时基础层打底，旧精灵自然被背景覆盖。
 *
 * 我们以前把 +0x18 当"功能未知"的空 stub（返回 0），applet 拿到的缓冲区
 * 指针是 0 → 背景无处可画 → 只能靠宿主帧缓冲"捡漏"留存 → 精灵移走后旧位置
 * 擦不掉（拖影）。 */
#define BASE_LAYER_BYTES ((uint32_t)LAYER_W * (uint32_t)LAYER_H * 2u)
static uint32_t g_base_layer = 0; /* 基础层缓冲的客户机地址（供诊断使用） */
uint32_t zm_display_GetBaseLayerBuffer(uc_engine *uc) {
  static bool inited = false;
  uint32_t base = g_base_layer;
  if (!inited) {
    inited = true;
    base = zm_pix_pool_alloc(BASE_LAYER_BYTES);
    g_base_layer = base;
    if (base) {
      static uint8_t zb[4096];
      for (uint32_t off = 0; off < BASE_LAYER_BYTES; off += sizeof(zb)) {
        uint32_t n = BASE_LAYER_BYTES - off;
        if (n > sizeof(zb))
          n = sizeof(zb);
        uc_mem_write(uc, base + off, zb, n);
      }
      log_info("[基础层] GetBaseLayerBuffer -> 0x%X (%dx%d RGB565, %u 字节)",
               base, LAYER_W, LAYER_H, BASE_LAYER_BYTES);
    } else {
      log_error("[基础层] 像素池不足（需 %u 字节），基础层不可用",
                BASE_LAYER_BYTES);
    }
  }
  return base;
}

/* 基础层缓冲地址（0 = 未分配）。供 fb_merge_layer 的探针检查其内容。 */
uint32_t zm_display_base_layer_addr(void) { return g_base_layer; }

/* +0x1C0 附近：RE 为 `ZMAEE_IDisplay_GetBaseLayerDepth()`（无参，默认 1） */
uint32_t zm_display_GetBaseLayerDepth(uc_engine *uc) {
  (void)uc;
  static uint32_t n = 0;
  if (n++ < 4)
    log_info("IDisplay.GetBaseLayerDepth -> 1 (%u 次)", n);
  return 1; /* RE：默认返回 1；unk_64B94 未落入 [24,32] 时也返回 1 */
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
  /*
   * applet 用这个值决定"我最多建几层"。
   *
   * 【2026-09 重要修正】以前恒返回 1（单层），导致 applet 只建一层、
   * 把背景和精灵全塞进同一层 —— 于是：
   *   - 层里 83.7% 是透明色（背景根本不在层里，只靠帧缓冲留存）
   *   - 精灵移走后旧位置擦不掉 → **残影**
   * 层数组本身支持 idx ∈ [0,15]（CreateLayerExt 校验 a2<=0xF），
   * 所以返回 1 是自缚手脚。真机值待定，用 ZM_MAX_LAYER 逐个试。
   */
  static int maxl = -1;
  if (maxl < 0) {
    const char *e = getenv("ZM_MAX_LAYER");
    maxl = (e && e[0]) ? atoi(e) : 16;
    if (maxl < 1)
      maxl = 1;
    if (maxl > 16)
      maxl = 16;
    log_info("IDisplay.GetMaxLayerCount -> %d（ZM_MAX_LAYER 可覆盖）", maxl);
  }
  static uint32_t ncall = 0;
  if (ncall++ < 8)
    log_info("IDisplay.GetMaxLayerCount(第 %u 次) -> %d", ncall, maxl);
  return (uint32_t)maxl;
}
uint32_t zm_display_CreateLayer(uc_engine *uc, uint32_t off, uint32_t r0,
                                uint32_t r1, uint32_t r2, uint32_t r3) {
  (void)off;
  /* +0x0C CreateLayer(display, idx, rect, fmt) —— 忠实实现见 zm_layer.c */
  return zm_layer_CreateLayer(uc, r0, r1, r2, r3);
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
  zm_display_slot_tick(0x1CU);
  (void)off;
  (void)r3;
  /* +0x1C GetLayerInfo(display, idx, out) —— 忠实实现见 zm_layer.c
   * RE：idx 只校验 >0xF；层项 +72（载荷 +0x24）为 0 即无效，返回 -4。 */
  return zm_layer_GetLayerInfo(uc, r0, r1, r2);
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
  /* RE：就是 `ldr r0, [r0, #8]`（与 SetActiveLayer 写入的同一字段）。
   * 以前硬编码返回 0，而 applet 用的是层 1 —— 自相矛盾。 */
  if (r0 == 0)
    return (uint32_t)-4;
  return uc_read32(uc, r0 + 8);
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
/* IDisplay 虚表 +0x20。
 *
 * 【重要修正】真机虚表（g_aee_display_vtbl，基址 0x63D40）逐槽对照：
 *   +0x18 FreeAllLayer   +0x1C GetLayerInfo   +0x20 **SetActiveLayer**
 *   +0x24 SetLayerPosition                    +0x28 Update
 *   +0x2C **UpdateEx**                        +0x30 GetActiveLayer
 * 也就是说这里**不是 clear(color)**，而是 `SetActiveLayer(display, idx)`！
 * 整个函数表里根本没有 clear —— 那是我们以前的误认。
 *
 * 这个错误极其致命：实测它每帧被调用 600+ 次、参数在 0/1 之间交替，
 * 那其实是 applet 在 **层 0 和层 1 之间来回切换活动层**。
 * 我们把它当成"清屏"只去写宿主 g_fb，于是：
 *   - 活动层索引从不由 applet 设定 → 所有绘制都落在同一个层上
 *   - 画进层 0 的背景全部落空（层 0 在我们这里没有缓冲）
 *   - 层里只剩精灵 → 帧缓冲只能靠上一帧累积 → 精灵移走后旧位置擦不掉（拖影）
 */
uint32_t zm_display_SetActiveLayer(uc_engine *uc, uint32_t display,
                                   uint32_t idx) {
  zm_display_slot_tick(0x20U);
  /* 层 0 = **基础层**。真机上它由 IDisplay_New/Init 预建，像素缓冲直接取全局
   * `ZMAEE_IDisplay_GetBaseLayerBuffer()`（= unk_64B60）。证据链：
   *   - `FreeAllLayer` 从 i=1 循环、永不释放层 0
   *   - `CreateLayer` 的 `(idx-1) > 0xE` 无符号比较会拒绝 idx=0
   *   - 实测 applet 每帧 SetActiveLayer(0) / (1) 交替，而 (0) 恒返回 -4
   * 我们没建层 0 → 画进层 0 的背景全部落空 → 层里只剩精灵 → 帧缓冲靠上一帧
   * 累积 → 精灵移走后旧位置擦不掉（拖影）。这里按需补建。 */
  /* 对照开关 ZM_NO_BASELAYER=1：不预建层 0，恢复"层 0 恒不存在"的旧行为。
   * 用来做 A/B —— 把两次运行的层图对比，就能看清"鱼到底被画进了哪个层"。 */
  static int no_base = -1;
  if (no_base < 0) {
    const char *e = getenv("ZM_NO_BASELAYER");
    no_base = (e && e[0] == '1') ? 1 : 0;
    if (no_base)
      log_info("[诊断] ZM_NO_BASELAYER=1：不建层 0（旧行为）");
  }
  if (!no_base && idx == 0 && display != 0 &&
      uc_read32(uc, zm_layer_payload(display, 0) + 0x24) == 0) {
    uint32_t buf = zm_display_GetBaseLayerBuffer(uc);
    if (buf) {
      uint32_t P = zm_layer_payload(display, 0);
      uc_write32(uc, P + 0x00, 1u);          /* 色深 1 = RGB565 */
      uc_write32(uc, P + 0x04, 0u);          /* x */
      uc_write32(uc, P + 0x08, 0u);          /* y */
      uc_write32(uc, P + 0x0C, (uint32_t)LAYER_W); /* 宽（兼 pitch） */
      uc_write32(uc, P + 0x10, (uint32_t)LAYER_H); /* 高 */
      uc_write32(uc, P + 0x14, 0u);          /* 裁剪 x */
      uc_write32(uc, P + 0x18, 0u);          /* 裁剪 y */
      uc_write32(uc, P + 0x1C, (uint32_t)LAYER_W); /* 宽副本 */
      uc_write32(uc, P + 0x20, (uint32_t)LAYER_H); /* 高副本 */
      uc_write32(uc, P + 0x24, buf);         /* 像素缓冲 */
      log_info("[基础层] 预建层 0: %dx%d RGB565 buf=0x%X", LAYER_W, LAYER_H,
               buf);
    }
  }
  uint32_t ret = zm_layer_SetActiveLayer(uc, display, idx);
  /* 注意：applet 每帧在层 0/1 之间来回切，所以这里**不能**按"索引变化"打日志，
   * 否则每帧两行。只在启动前几次 + 之后按固定间隔汇总。 */
  static uint32_t n = 0;
  n++;
  if (n <= 8)
    log_info("IDisplay.SetActiveLayer(%u) -> %d（第 %u 次）", idx, (int)ret, n);
  else if ((n % 600) == 0)
    log_info("[探针]SetActiveLayer 累计 %u 次，当前活动层=%u（0/1 每帧交替属正常）",
             n, uc_read32(uc, DISPLAY + 8));
  return ret;
}

/* +0x2C：fillRect(rect_ptr,...)。
 * 注意：applet 在绘制末尾调用 fillRect(全屏rect, 1, &0)，语义不明
 * （疑似 invalidate / 带透明度混合，颜色=0 透明）。为避免用黑色覆盖
 * 整张画面，这里保持空实现。 */
uint32_t zm_display_UpdateEx(uc_engine *uc, uint32_t display, uint32_t rect_ptr,
                             uint32_t count, uint32_t list_ptr) {
  /* 真机虚表 +0x2C = ZMAEE_IDisplay_UpdateEx(display, rect, count, layerIdList)
   *
   * RE：把 layerIdList 里列出的层，按列表顺序依次合成到"锁定的帧缓冲"
   * （LockFrameBuffer 返回的屏幕位图）的 rect 区域内。
   * 也就是说 —— **合成哪些层、按什么顺序，是 applet 传进来的参数决定的**，
   * 不是我们该猜的。
   *
   * 以前这里只接了一个参数（当成 fillRect(rect_ptr)），count 和层列表全丢了。
   * 本函数现在只做观测：把每次调用的 rect / 层数 / 层列表打出来。
   * 实际合成仍由 fb_present → fb_composite_layers 完成。 */
  zm_display_slot_tick(0x2Cu);
  (void)display;
  static uint32_t n = 0;
  static uint32_t seq_hist[256]; /* 以"层号序列"做键（<=4 层，每层 4bit） */
  static uint32_t seq_total = 0;
  n++;
  /* 统计每次调用实际合成的是哪些层、按什么顺序 —— 这就是叠加顺序的权威来源 */
  {
    uint32_t lst[8] = {0};
    if (list_ptr && count > 0 && count <= 8)
      uc_mem_read(uc, list_ptr, lst, (size_t)count * 4u);
    uint32_t key = 0;
    for (uint32_t i = 0; i < count && i < 4u; i++)
      key = (key << 4) | (lst[i] & 0xFu);
    key = (key << 4) | (count & 0xFu); /* 低 4 位存层数 */
    seq_hist[key & 0xFFu]++;
    if ((++seq_total % 300u) == 0u) {
      log_info("[UpdateEx] 累计 %u 次，层号序列分布（低位=层数，高位从第 1 层起）：",
               seq_total);
      for (uint32_t k = 0; k < 256u; k++)
        if (seq_hist[k])
          log_info("   0x%02X : %u 次", k, seq_hist[k]);
    }
  }
  if (n <= 8) {
    uint32_t r[4] = {0};
    if (rect_ptr)
      uc_mem_read(uc, rect_ptr, r, sizeof(r));
    uint32_t lst[8] = {0};
    if (list_ptr && count > 0 && count <= 8)
      uc_mem_read(uc, list_ptr, lst, (size_t)count * 4u);
    /* 先把原始寄存器打出来 —— 参数位置不能靠猜 */
    uint32_t m1 = 0, m2 = 0;
    uc_mem_read(uc, rect_ptr + 16u, &m1, 4);
    if (list_ptr)
      uc_mem_read(uc, list_ptr, &m2, 4);
    log_info("[UpdateEx #%u] 原始: r0(display)=0x%X r1=0x%X r2=0x%X r3=0x%X | "
             "rect[0..3]={%d,%d,%d,%d} rect[4]=0x%X mem[r3]=0x%X",
             n, display, rect_ptr, count, list_ptr, (int)r[0], (int)r[1], (int)r[2],
             (int)r[3], m1, m2);
    char buf[160];
    int off = snprintf(buf, sizeof(buf), "[UpdateEx #%u] rect={%d,%d,%d,%d} count=%u 层列表=",
                       n, (int)r[0], (int)r[1], (int)r[2], (int)r[3], count);
    for (uint32_t i = 0; i < count && i < 8u && off > 0 &&
                         off < (int)sizeof(buf) - 8; i++)
      off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%u ", lst[i]);
    log_info("%s（列表顺序 = 合成顺序，后者叠加在前者之上）", buf);
  }
  return 0;
}

/* +0x40：commit，提交帧缓冲 */
uint32_t zm_display_commit(uc_engine *uc) {
  zm_display_slot_tick(0x40U);
  (void)uc;
  fb_commit();
  return 0;
}

/* +0x48：getWidth，返回屏幕宽度。
 * sub_8062C 用返回值+8 作为文本布局宽度；
 * sub_80790 用返回值+a2 作为文本区域宽度。 */
uint32_t zm_display_getWidth(uc_engine *uc) {
  zm_display_slot_tick(0x48U);
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
  zm_display_slot_tick(0x4CU);
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
  zm_display_slot_tick(0x50U);
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
/* 忠实实现 ZMAEE_IDisplay_FillRect：写到**活动层**的像素缓冲。
 *
 * 反编译要点：
 *   v7  = &result[13 * result[2]]      // result[2] = *(IDisplay+8) 活动层
 *   v20 = v7[9]   色深       v22 = v7[18] 像素缓冲
 *   v11 = v7[12]  pitch      v14 = v7[13] 高
 *   v8  = v7[14]  裁剪左      v9  = v7[15] 裁剪上
 *   v10 = v7[17]  高副本      v16 = v7[16] 宽副本
 *   色深 1    → RGB565: ((c&0xF80000)>>8)|((c&0xFC00)>>5)|((c&0xFF)>>3)
 *   色深 2..4 → 32bit: c | 0xFF000000
 *
 * 以前只写宿主 g_fb —— 而 g_fb 每次 present 都会被 fb_merge_layer 整块
 * 覆盖，等于什么都没画：applet 每帧的"清屏"完全失效 → 精灵移动后旧位置
 * 擦不掉（拖影）。 */
static void layer_fill(uc_engine *uc, int x, int y, int w, int h,
                       uint32_t color) {
  uint32_t active = uc_read32(uc, DISPLAY + 8);
  zm_layer_t L;
  if (zm_layer_get(uc, DISPLAY, active, &L) != 0)
    return; /* 活动层不存在（还没 CreateLayer） */
  uint32_t depth = L.fmt;
  int pitch = (int)L.w;
  int lh = (int)L.h;
  int cl = (int)L.cx;
  int ct = (int)L.cy;
  int cw = (int)L.cw;
  int ch = (int)L.ch;
  uint32_t buf = L.buf;
  if (!buf || pitch <= 0)
    return;
  if (depth != 1 && (depth < 2 || depth > 4))
    return;
  int bpp = (depth == 1) ? 2 : 4;

  /* 层可视区 [max(cl,0), cl+cw] ∩ [0,pitch]，再与请求矩形相交 */
  int l = cl > 0 ? cl : 0;
  int r = cl + cw;
  if (r > pitch)
    r = pitch;
  int t = ct > 0 ? ct : 0;
  int b = ct + ch;
  if (b > lh)
    b = lh;
  int x0 = x > l ? x : l;
  int y0 = y > t ? y : t;
  int x1 = x + w < r ? x + w : r;
  int y1 = y + h < b ? y + h : b;
  if (x1 <= x0 || y1 <= y0)
    return;
  int npx = x1 - x0;
  if (npx > 1024)
    return;
  {
    /* 细粒度探针：确认填充到底有没有真的写进层缓冲。
     * 前 3 次打印全部字段，其后每 200 次汇报累计写入像素数。 */
    static uint32_t ncall = 0, nwritten = 0;
    static int detail = 3;
    nwritten += (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
    ncall++;
    if (detail > 0) {
      detail--;
      log_info("[FillRect详查] 层=%u fmt=%u 请求(%d,%d,%d,%d) 色=0x%08X | "
               "L.pos(%d,%d) L.size(%d,%d) clip(%d,%d,%d,%d) buf=0x%X -> "
               "有效[%d,%d]-[%d,%d] = %d 像素/行",
               active, depth, x, y, w, h, color, (int)L.x, (int)L.y, pitch, lh, cl,
               ct, cw, ch, buf, x0, y0, x1, y1, npx);
    } else if ((ncall % 200) == 0) {
      log_info("[FillRect详查] 累计 %u 次填充, 累计写入 %u 像素", ncall, nwritten);
    }
  }

  if (depth == 1) {
    static uint16_t row16[1024];
    uint16_t c16 = (uint16_t)(((color & 0xF80000u) >> 8) |
                              ((color & 0xFC00u) >> 5) | ((color & 0xFFu) >> 3));
    /* 填充色 == 层透明色 → applet 是在"把这片区域清成透明"
     * （实测它每帧都调 clear(1) → FillRect(0,0,240,320, 0xFFFC00FF)）。
     *
     * 把透明色**真的写进层**才是正确语义：合成时该像素命中
     * transparent-key 会被跳过、透出帧缓冲里原有的背景，于是"清空"生效
     *   - 精灵移动后的旧位置被擦掉      → 拖影消失
     *   - 更早某屏残留的压暗遮罩被清掉  → 按钮"黑底"(RGB565 0x2945)消失
     *
     * 【2026-09 复核】这里以前直接 return 跳过，理由是"怕刷成满屏品红"。
     * 那个现象的真正原因不是本分支，而是 SetTransColor 把透明色写到了
     * 【层 0】而精灵在【层 1】—— 层 1 的 +0x2C 恒为 0，合成时 use_tc=0
     * 整行直写，品红当然全屏。按 RE 修正到活动层后本分支即可放开。
     *
     * 对照开关：ZM_NO_CLEAR=1 恢复"填透明色即跳过"的旧行为，专供 A/B 对比。 */
    static int no_clear = -1;
    if (no_clear < 0) {
      const char *e = getenv("ZM_NO_CLEAR");
      no_clear = (e && e[0] == '1') ? 1 : 0;
    }
    if (no_clear && L.tenable && c16 == to_rgb565(L.tcolor)) {
      (void)c16;
      return;
    }
    for (int i = 0; i < npx; i++)
      row16[i] = c16;
    for (int yy = y0; yy < y1; yy++)
      uc_mem_write(uc, buf + (uint32_t)((yy * pitch + x0) * 2), row16,
                   (size_t)npx * 2u);
  } else {
    static uint32_t row32[1024];
    uint32_t c32 = color | 0xFF000000u;
    for (int i = 0; i < npx; i++)
      row32[i] = c32;
    for (int yy = y0; yy < y1; yy++)
      uc_mem_write(uc, buf + (uint32_t)((yy * pitch + x0) * 4), row32,
                   (size_t)npx * 4u);
  }
}

uint32_t zm_display_FillRect(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                             uint32_t sp) {
  zm_display_slot_tick(0x70U);
  int h = (int)uc_read32(uc, sp);
  uint32_t color = uc_read32(uc, sp + 4);
  static uint32_t n = 0;
  if ((n++ % 200) == 0)
    log_info("IDisplay.FillRect(%d,%d,%d,%d, 0x%08X)（第 %u 次）", (int)x, (int)y,
             (int)w, h, color, n);
  /* 实测（层结构已修正后仍然如此）：applet 每帧都调
   *   FillRect(0,0,240,320, 0xFFFC00FF)
   * 把层清成透明色，且**不把背景重画进层**——实测层缓冲 83.9% 是 0xF81F。
   * 也就是说背景不在这一层里；在只有一块缓冲的前提下启用下面的真实填充
   * 会把背景一起擦掉，变成满屏品红。故默认关闭，ZM_FILLRECT=1 才启用
   * （用于将来补上"底层背景缓冲"后验证）。 */
  {
    static int en = -1;
    if (en < 0) {
      const char *e = getenv("ZM_FILLRECT");
      en = (e && e[0] == '0') ? 0 : 1; /* 默认开；ZM_FILLRECT=0 关 */
      log_info("IDisplay.FillRect 写层缓冲 = %s", en ? "开" : "关（默认）");
    }
    if (en)
      layer_fill(uc, (int)x, (int)y, (int)w, h, color);
  }
  return 0;
}

/* ---- 未实测槽（stub） ---- */

uint32_t zm_display_SetTransColor(uc_engine *uc, uint32_t r0, uint32_t r1,
                                  uint32_t r2) {
  zm_display_slot_tick(0x54U);
  (void)r0;
  /*
   * RE：ZMAEE_IDisplay_SetTransColor(a1, a2, a3)
   *   v3 = a1 + 52 * *(_DWORD *)(a1 + 8);   // ← **活动层**（a1+8 是活动层索引）
   *   *(_DWORD *)(v3 + 84) = a3;            // 载荷 +0x30 = 透明色
   *   *(_DWORD *)(v3 + 80) = a2;            // 载荷 +0x2C = 启用标志
   * 即：a2 → 载荷 +0x2C（非 0 = 启用透明色），a3 → 载荷 +0x30（颜色）。
   *
   * 【2026-09 修正】以前这里写死 `DISPLAY + 36 + 0x2C/0x30`，那是**层 0**。
   * 实测本 applet 的精灵全画在**层 1**（GetLayerInfo 6 次全是层=1、
   * buf=0x7A8000），于是层 1 的 +0x2C 永远是 0 → zb_composite_layers 里
   * use_tc=0 → 整行直写，透明画不出来、旧像素擦不掉。
   */
  if (!g_trans_forced)
    g_trans_color = r1;
  g_trans_color2 = r2;

  uint32_t active = uc_read32(uc, DISPLAY + 8);
  if (active > 0xF)
    active = 0;
  uint32_t P = zm_layer_payload(DISPLAY, active);
  uc_write32(uc, P + 0x2C, r1); /* 启用标志 */
  uc_write32(uc, P + 0x30, r2); /* 透明色（ARGB，取值时转 RGB565） */
  log_info("IDisplay.SetTransColor(启用=0x%08X, 色=0x%08X) -> 层%u (+0x2C/+0x30) -> "
           "RGB565 key=0x%04X%s",
           r1, r2, active, to_rgb565(r2),
           g_trans_forced ? " （已被 ZM_TRANS_KEY 覆盖，忽略）" : "");
  return 0;
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
  /* RE：ZMAEE_IDisplay_CreateBitmap(display, w, h, fmt, a5, a6, out)
   *       → ZMAEE_IBitmap_Create(w, h, 512-调色板空间, fmt, a5, a6, out)
   * IBitmap_Create 的关键语义：
   *   bpp = ZMCF2BytsPerPixel(fmt)          // 0→1, 1→2, 2/3/4→4
   *   px  = (bpp*w*h + 3) & ~3
   *   fmt != 0 → 只分配 px（非索引色不需要调色板）
   *   fmt == 0 → 分配 px + 调色板；调色板指针 = 像素指针 + px
   *   字段：+8 宽 +12 高 +16 格式 +20 透明色(-1)
   *         +24 有无调色板 +28 调色板指针 +32/40 调色板大小 +36 像素指针
   * out 是**第 7 个参数**（sp+8）。以前这里是空 stub，既不建位图也不写 out，
   * applet 拿到的 out 是栈上脏值 —— 自建离屏位图的路径整条走不通。 */
  uint32_t out_ptr = getArg(uc, 6);
  if (r0 == 0) {
    if (out_ptr)
      uc_write32(uc, out_ptr, 0);
    return (uint32_t)-4;
  }
  if (out_ptr)
    uc_write32(uc, out_ptr, 0);

  int w = (int)r1, h = (int)r2, fmt = (int)r3;
  static const int bpp_tab[5] = {1, 2, 4, 4, 4}; /* RE：ZMCF2BytsPerPixel */
  int bpp = (fmt >= 0 && fmt <= 4) ? bpp_tab[fmt] : 0;
  if (w <= 0 || h <= 0 || !bpp || !out_ptr)
    return (uint32_t)-4;

  uint32_t px = ((uint32_t)w * (uint32_t)h * (uint32_t)bpp + 3u) & ~3u;
  uint32_t palsz = (fmt == 0) ? 512u : 0u; /* CreateBitmap 传的调色板空间 */
  uint32_t gpx = zm_pix_pool_alloc(px + palsz);
  if (!gpx)
    return (uint32_t)-2;
  {
    static uint8_t zb[1024];
    for (uint32_t off = 0; off < px + palsz; off += sizeof(zb)) {
      uint32_t n = px + palsz - off;
      if (n > sizeof(zb))
        n = sizeof(zb);
      uc_mem_write(uc, gpx + off, zb, n);
    }
  }

  static uint32_t slot = 0;
  uint32_t obj = BITMAP_POOL + (slot++ % BITMAP_SLOT_COUNT) * BITMAP_SLOT_SIZE;
  uc_write32(uc, obj, BITMAP_VT_ADDR);
  uc_write32(uc, obj + 4, 1);
  uc_write32(uc, obj + 8, (uint32_t)w);
  uc_write32(uc, obj + 12, (uint32_t)h);
  uc_write32(uc, obj + 16, (uint32_t)fmt);
  uc_write32(uc, obj + 20, 0xFFFFFFFFu); /* 透明色初值 -1 */
  uc_write32(uc, obj + 24, (fmt == 0) ? 1u : 0u);
  uc_write32(uc, obj + 28, (fmt == 0) ? gpx + px : 0u); /* 调色板紧跟像素 */
  uc_write32(uc, obj + 32, palsz);
  uc_write32(uc, obj + 36, gpx);
  uc_write32(uc, obj + 40, palsz);
  uc_write32(uc, out_ptr, obj);
  log_info("CreateBitmap(%dx%d fmt=%d) -> 0x%X pix@0x%X pal@0x%X", w, h, fmt, obj,
           gpx, (fmt == 0) ? gpx + px : 0u);
  return 0;
}
uint32_t zm_display_LoadBitmap(uc_engine *uc, uint32_t r0, uint32_t r1) {
  /* RE：ZMAEE_IDisplay_LoadBitmap(display, name, a3, a4, out)
   *       → ZMAEE_IBitmap_LoadFile(out, name)
   * 文件即 .zbmp（魔数 "ZMBM"），布局（反编译 + 实测三个文件全部吻合）：
   *   +0  魔数 "ZMBM"
   *   +4  (高<<16) | 宽
   *   +8  (调色板标志<<16) | 颜色格式
   *   +12 透明色            （实例里为 0x00FF00FF，alpha=0 → 透明）
   *   +16 调色板大小
   *   +20 像素数据（w*h*bpp，4 字节对齐），其后紧跟调色板
   * 以前这里返回无像素的 BITMAP 单例 —— applet 拿到 w/h 全 0 的位图，
   * 画出来自然什么都没有（实测 1374 次调用全落空）。 */
  uint32_t out_ptr = getArg(uc, 4);
  if (out_ptr == 0)
    return (uint32_t)-4;
  uc_write32(uc, out_ptr, 0);
  if (r0 == 0 || r1 == 0)
    return (uint32_t)-4;

  char name[256];
  for (int i = 0; i < 255; i++) {
    uint8_t ch = 0;
    if (uc_mem_read(uc, r1 + (uint32_t)i, &ch, 1) != UC_ERR_OK) {
      name[i] = 0;
      break;
    }
    name[i] = (char)ch;
    if (!ch)
      break;
    name[i + 1] = 0;
  }
  name[255] = 0;

  uint8_t *buf = NULL;
  size_t len = 0;
  if (zm_fs_read_file(name, &buf, &len) != 0 || !buf || len < 20) {
    log_debug("LoadBitmap(\"%s\") 读取失败", name);
    free(buf);
    return (uint32_t)-1;
  }
  if (memcmp(buf, "ZMBM", 4) != 0) {
    log_debug("LoadBitmap(\"%s\") 非 ZMBM", name);
    free(buf);
    return (uint32_t)-1;
  }

  uint32_t wh, fpf, trans, palsize;
  memcpy(&wh, buf + 4, 4);
  memcpy(&fpf, buf + 8, 4);
  memcpy(&trans, buf + 12, 4);
  memcpy(&palsize, buf + 16, 4);
  int w = (int)(wh & 0xFFFFu), h = (int)(wh >> 16);
  int fmt = (int)(fpf & 0xFFFFu), palflag = (int)(fpf >> 16);
  static const int bpp_tab[5] = {1, 2, 4, 4, 4}; /* RE：ZMCF2BytsPerPixel */
  int bpp = (fmt >= 0 && fmt <= 4) ? bpp_tab[fmt] : 0;
  if (!w || !h || !bpp) {
    free(buf);
    return (uint32_t)-1;
  }
  size_t px = ((size_t)w * (size_t)h * (size_t)bpp + 3u) & ~(size_t)3u;
  if (20u + px > len) {
    free(buf);
    return (uint32_t)-1;
  }

  uint32_t gpx = zm_pix_pool_alloc((uint32_t)px);
  if (!gpx) {
    free(buf);
    return (uint32_t)-1;
  }
  uc_mem_write(uc, gpx, buf + 20, px);

  uint32_t gpal = 0;
  if (palflag && palsize && 20u + px + palsize <= len) {
    gpal = zm_pix_pool_alloc(palsize);
    if (gpal)
      uc_mem_write(uc, gpal, buf + 20 + px, palsize);
  }
  free(buf);

  /* 建 IBitmap 对象（字段与 ZMAEE_IBitmap_LoadFile 逐条对应） */
  static uint32_t slot = 0;
  uint32_t obj = BITMAP_POOL + (slot++ % BITMAP_SLOT_COUNT) * BITMAP_SLOT_SIZE;
  uc_write32(uc, obj, BITMAP_VT_ADDR);
  uc_write32(uc, obj + 4, 1);                                /* 引用计数 */
  uc_write32(uc, obj + 8, (uint32_t)w);                      /* 宽 */
  uc_write32(uc, obj + 12, (uint32_t)h);                     /* 高 */
  uc_write32(uc, obj + 16, (uint32_t)fmt);                   /* 颜色格式 */
  uc_write32(uc, obj + 20, trans);                           /* 透明色 */
  uc_write32(uc, obj + 24, palflag ? 1u : 0u);               /* 调色板标志 */
  uc_write32(uc, obj + 28, gpal);                            /* 调色板指针 */
  uc_write32(uc, obj + 32, palflag ? palsize : 0u);          /* 调色板大小 */
  uc_write32(uc, obj + 36, gpx);                             /* 像素指针 */
  uc_write32(uc, obj + 40, palflag ? palsize : 0u);
  uc_write32(uc, out_ptr, obj);
  log_info("LoadBitmap(\"%s\") -> 0x%X %dx%d fmt=%d bpp=%d trans=0x%08X pix@0x%X",
           name, obj, w, h, fmt, bpp, trans, gpx);
  return 0;
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
  /* RE：ZMAEE_IDisplay_StretchBlt(display, a2, a3, a4, a5)
   *   → ZMAEE_StretchBlt(&display[13*display[2] + 9], a2, a3, a4, a5)
   * 目标 = 活动层载荷；a1[9] = 层像素缓冲，a1[5..8] = 裁剪区。
   * a2/a3/a4 是调用方在栈上构造的描述结构。
   * 本函数目前是空实现 —— applet 实测会调用它，等于绘制被丢弃。
   * 先把三个参数结构 dump 出来，确认它画的是什么。 */
  static uint32_t n = 0;
  n++;
  if (n <= 6) {
    uint32_t act = uc_read32(uc, r0 + 8);
    uint32_t P = r0 + 52u * act + 36u;
    log_info("StretchBlt #%u display=0x%X 活动层=%u 层载荷=0x%X "
             "层[fmt=0x%X w=0x%X h=0x%X buf=0x%X] a2=0x%X a3=0x%X a4=0x%X",
             n, r0, act, P, uc_read32(uc, P), uc_read32(uc, P + 0x0C),
             uc_read32(uc, P + 0x10), uc_read32(uc, P + 0x24), r1, r2, r3);
    const char *nm[3] = {"a2", "a3", "a4"};
    uint32_t pa[3] = {r1, r2, r3};
    for (int i = 0; i < 3; i++) {
      uint32_t raw[10] = {0};
      if (uc_mem_read(uc, pa[i], raw, sizeof(raw)) != UC_ERR_OK)
        continue;
      log_info("  StretchBlt %s@0x%X =", nm[i], pa[i]);
      log_info("     [0..3] 0x%08X 0x%08X 0x%08X 0x%08X", raw[0], raw[1], raw[2],
               raw[3]);
      log_info("     [4..7] 0x%08X 0x%08X 0x%08X 0x%08X", raw[4], raw[5], raw[6],
               raw[7]);
      log_info("     [8..9] 0x%08X 0x%08X", raw[8], raw[9]);
    }

  }
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
  (void)uc;
  (void)off;
  (void)r0;
  (void)r1;
  (void)r2;
  (void)r3;
  /* RE：真机该函数体就是 `return -1;`（不支持 DM 层）。
   * 以前返回 0 —— 若 applet 用 ">= 0" 判断句柄有效性，0 会被当成
   * **有效句柄**而走上一条本不该走的路径。 */
  return (uint32_t)-1;
}
uint32_t zm_display_RelevanceLayer(uc_engine *uc, uint32_t off, uint32_t r0,
                                   uint32_t r1, uint32_t r2, uint32_t r3) {
  (void)uc;
  (void)off;
  (void)r0;
  (void)r1;
  (void)r2;
  (void)r3;
  return (uint32_t)-1; /* RE：真机同样 `return -1;` */
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
  /* RE：ZMAEE_IBitmap_SetTransColor(bitmap, color)
   *       if (bitmap != 0) *(bitmap + 20) = color;
   *       return bitmap;
   * 这个字段就是 GDI 决定"走 mask 还是 copy"的依据：
   *   GDI_BitBlt_Ext 里 v8 = a7 & (~*(info + 12) >> 31)，
   *   而 info = memcpy(bitmap + 8, 32)，故 info[3] 即 bitmap+20 的透明色。
   *     透明色 = -1（IBitmap_Create 的初值）→ ~(-1)>>31 = 0 → 走 copy（不透明）
   *     透明色 ≥ 0                        → 结果为 1       → 走 mask（透明生效）
   * 以前这里是空实现、把颜色丢掉 —— applet 想让某张图透明时完全无效，
   * 于是那张图会带着背景色块被整块贴上去。 */
  if (r0 != 0)
    uc_write32(uc, r0 + 20, r1);
  {
    static uint32_t n = 0;
    if (n++ < 8)
      log_info("IBitmap.SetTransColor(bitmap=0x%X, color=0x%08X)（第 %u 次）", r0, r1,
               n);
  }
  return r0;
}
uint32_t zm_bitmap_GetInfo(uc_engine *uc, uint32_t r0, uint32_t r1) {
  /* RE：ZMAEE_IBitmap_GetInfo(bitmap, out) 就是
   *     zmaee_memcpy(out, bitmap + 8, 32);
   * 即把 IBitmap 的 +8..+40 八个 dword 原样交给调用方（GDI 直接消费它，
   * 其中 out[2]=+16 颜色格式、out[3]=+20 透明色、out[7]=+36 像素指针）。
   * 以前只写宽高、其余留 0 → GDI 拿到空像素指针，8bit 调色板与 32bit
   * alpha 两条路径全废。 */
  if (r0 == 0 || r1 == 0)
    return (uint32_t)-4;
  uint8_t buf[32];
  if (uc_mem_read(uc, r0 + 8, buf, sizeof(buf)) != UC_ERR_OK)
    return (uint32_t)-4;
  uc_mem_write(uc, r1, buf, sizeof(buf));
  return 0;
}

uint32_t zm_bitmap_sub_25F78(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3) {
  /* RE：int sub_25F78(int a1) { return a1 ? *(a1 + 20) : 0; }
   * 即取位图的透明色（与 SetTransColor 写的是同一字段）。 */
  (void)off;
  (void)r1;
  (void)r2;
  (void)r3;
  if (r0 == 0)
    return 0;
  return uc_read32(uc, r0 + 20);
}

uint32_t zm_bitmap_sub_25F84(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3) {
  /* RE：sub_25F84(bitmap, out0, out1)
   *   v3 = (bitmap && *(bitmap+24) 有调色板标志) ? *(bitmap+32) 调色板大小 : 0
   *   if (out0) { *out0 = 0; }        // 固件里这个出参恒写 0
   *   if (out1) *out1 = v3;
   *   返回 0（有 out0 时）；否则返回原 bitmap。 */
  (void)off;
  (void)r3;
  uint32_t v3 = 0;
  if (r0 != 0 && uc_read32(uc, r0 + 24) != 0)
    v3 = uc_read32(uc, r0 + 32);
  if (r1 != 0) {
    uc_write32(uc, r1, 0);
    if (r2 != 0)
      uc_write32(uc, r2, v3);
    return 0;
  }
  if (r2 != 0)
    uc_write32(uc, r2, v3);
  return r0;
}

uint32_t zm_bitmap_sub_25FF8(uc_engine *uc, uint32_t off, uint32_t r0,
                             uint32_t r1, uint32_t r2, uint32_t r3) {
  /* RE：sub_25FF8(bitmap, src, len) —— 8bit 索引位图的调色板写入
   *   bitmap==0 / src==0 / len<=0            → -4
   *   *(bitmap+24) == 0（无调色板）           → -1
   *   *(bitmap+28) == 0（调色板指针为空）      → -1
   *   cap = *(bitmap+40)
   *   len >= cap → 长度=cap，memcpy(cap)
   *   否则       → 长度=len，memcpy(len) 并把尾部清零
   *   长度写回 *(bitmap+8) */
  (void)off;
  (void)r3;
  if (r0 == 0 || r1 == 0 || (int)r2 <= 0)
    return (uint32_t)-4;
  if (uc_read32(uc, r0 + 24) == 0)
    return (uint32_t)-1;
  uint32_t pal = uc_read32(uc, r0 + 28);
  if (pal == 0)
    return (uint32_t)-1;
  uint32_t cap = uc_read32(uc, r0 + 40);
  uint32_t n = ((uint32_t)r2 >= cap) ? cap : (uint32_t)r2;
  {
    static uint8_t tmp[4096];
    uint32_t done = 0;
    while (done < n) {
      uint32_t c = n - done;
      if (c > sizeof(tmp))
        c = sizeof(tmp);
      if (uc_mem_read(uc, r1 + done, tmp, c) != UC_ERR_OK)
        return (uint32_t)-1;
      uc_mem_write(uc, pal + done, tmp, c);
      done += c;
    }
  }
  if ((uint32_t)r2 < cap) {
    uint8_t zero[512] = {0};
    uint32_t left = cap - n;
    uint32_t off2 = pal + n;
    while (left > 0) {
      uint32_t c = left > sizeof(zero) ? sizeof(zero) : left;
      uc_mem_write(uc, off2, zero, c);
      off2 += c;
      left -= c;
    }
  }
  uint32_t used = ((uint32_t)r2 >= cap) ? cap : (uint32_t)r2;
  uc_write32(uc, r0 + 8, used); /* RE：写回 *(bitmap+8) */
  return 0;
}

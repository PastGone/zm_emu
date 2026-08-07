#include "zm_gfx.h"
#include "zm_layer.h"

#include "../../emu.h"
#include "../../log/log.h"
#include "../../tool/uc_helper.h"
#include "unicorn/unicorn.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <stdlib.h>
#include <string.h>

/* ---------- 渲染器（SDL2 + SDL_ttf） ----------
 * 设计要点：
 *  - 真正的"显存"是 zm_layer 里的客户机 RGB565 图层缓冲，
 *    所有绘制原语都软件光栅化写进去（applet 也能直接写）；
 *  - SDL 只负责三件事：把各层合成后的 ARGB8888 帧送显、
 *    用 SDL_ttf 把文字栅格化成位图、以及处理窗口事件；
 *  - 无头模式用 SDL_VIDEODRIVER=dummy，绘制照常发生，只是不显示；
 *    zm_gfx_save_bmp 可把合成结果落盘，用于自动化校验。
 */

/* 候选字体：优先带中文的 wqy / Noto CJK，退化到拉丁字体。
 * applet 的文本大多是 ASCII 数字与英文，但 00000405 等会画中文标题。 */
static const char *k_font_candidates[] = {
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
};

static SDL_Window *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static SDL_Texture *g_tex = NULL;  /* STREAMING，用来把 g_frame 送显 */
static uint32_t *g_frame = NULL;   /* 宿主机 ARGB8888 合成帧 */
static int g_w = 0;
static int g_h = 0;
static bool g_ready = false;
static const char *g_font_path = NULL;

/* 字体按 font_size 缓存，size 变化时重新打开 */
static TTF_Font *g_font = NULL;
static int g_font_size = 0;

/* 绘制计数，用于日志与"确实画了东西"的判定 */
static uint32_t g_draw_ops = 0;

/* ---------- 小工具 ---------- */

static const char *pick_font_path(void) {
  if (g_font_path)
    return g_font_path;
  const char *env = getenv("ZM_FONT");
  if (env && *env) {
    FILE *f = fopen(env, "rb");
    if (f) {
      fclose(f);
      g_font_path = env;
      return g_font_path;
    }
    log_warn("ZM_FONT 指定的字体不存在: %s", env);
  }
  for (size_t i = 0; i < sizeof(k_font_candidates) / sizeof(*k_font_candidates);
       i++) {
    FILE *f = fopen(k_font_candidates[i], "rb");
    if (f) {
      fclose(f);
      g_font_path = k_font_candidates[i];
      log_info("使用字体: %s", g_font_path);
      return g_font_path;
    }
  }
  log_warn("系统中未找到可用字体，文本将不会被绘制");
  return NULL;
}

/* 根据 font_size 打开（或复用）字体 */
static TTF_Font *get_font(int font_size) {
  if (font_size <= 0)
    font_size = 16;
  /* applet 传入的 font_size 可能是 font_id(1,2) 而非像素值；
   * 小于 8 时视为 font_id，映射到可读的像素大小。 */
  if (font_size < 8)
    font_size = 14;
  if (font_size > 128)
    font_size = 128;
  if (g_font && g_font_size == font_size)
    return g_font;

  const char *path = pick_font_path();
  if (!path)
    return NULL;

  if (g_font) {
    TTF_CloseFont(g_font);
    g_font = NULL;
  }
  g_font = TTF_OpenFont(path, font_size);
  if (!g_font) {
    log_error("TTF_OpenFont(%s, %d) failed: %s", path, font_size,
              TTF_GetError());
    return NULL;
  }
  g_font_size = font_size;
  return g_font;
}

/* ---------- 画布操作（全部落到当前活动图层） ---------- */

static void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
  if (!g_ready || w <= 0 || h <= 0)
    return;
  zm_layer_fill(g_uc, zm_layer_active(), x, y, w, h, color);
  g_draw_ops++;
}

static void fb_draw_rect(int x, int y, int w, int h, uint32_t color) {
  if (!g_ready || w <= 0 || h <= 0)
    return;
  zm_layer_frame(g_uc, zm_layer_active(), x, y, w, h, color);
  g_draw_ops++;
}

/* 在 rect_ptr 指向的矩形 {x,y,w,h} 内居中绘制文本 */
static void fb_draw_text(uc_engine *uc, uint32_t rect_ptr, const char *text,
                         uint32_t color, int font_size) {
  if (!g_ready || !rect_ptr || !text || !text[0])
    return;

  int rx = (int)uc_read32(uc, rect_ptr);
  int ry = (int)uc_read32(uc, rect_ptr + 4);
  int rw = (int)uc_read32(uc, rect_ptr + 8);
  int rh = (int)uc_read32(uc, rect_ptr + 12);
  if (rw <= 0 || rh <= 0) {
    /* 有些 applet 传的是一个点，退化成"以该点为左上角自由绘制" */
    rw = g_w - rx;
    rh = g_h - ry;
    if (rw <= 0 || rh <= 0)
      return;
  }

  TTF_Font *font = get_font(font_size);
  if (!font)
    return;

  SDL_Color fg;
  fg.a = 0xFF;
  fg.r = (uint8_t)((color >> 16) & 0xFF);
  fg.g = (uint8_t)((color >> 8) & 0xFF);
  fg.b = (uint8_t)(color & 0xFF);

  SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, fg);
  if (!surf) {
    log_debug("TTF_RenderUTF8_Blended('%s') failed: %s", text, TTF_GetError());
    return;
  }
  SDL_Surface *conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ARGB8888, 0);
  SDL_FreeSurface(surf);
  if (!conv)
    return;

  int dx = rx + (rw - conv->w) / 2;
  int dy = ry + (rh - conv->h) / 2;
  zm_layer_blit_argb(uc, zm_layer_active(), dx, dy, (const uint32_t *)conv->pixels,
                     conv->w, conv->h, conv->pitch / 4, rx, ry, rw, rh);
  SDL_FreeSurface(conv);
  g_draw_ops++;
}

static void pump_events(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) {
      /* 不再 exit(0)：只是请求收尾，由主循环走正常的资源释放路径 */
      log_info("收到 SDL_QUIT，请求结束模拟");
      g_stop_requested = 1;
      if (g_uc)
        uc_emu_stop(g_uc);
    }
  }
}

/* ---------- 生命周期 ---------- */

int zm_gfx_init(void) {
  if (g_ready)
    return 0;

  if (g_header.ScreenW == 0 || g_header.ScreenW > 4096)
    g_header.ScreenW = 240;
  if (g_header.ScreenH == 0 || g_header.ScreenH > 4096)
    g_header.ScreenH = 320;
  g_w = (int)g_header.ScreenW;
  g_h = (int)g_header.ScreenH;

  if (g_headless) {
    /* 无窗口：dummy 视频驱动 + 软件渲染，绘制流程完全一致 */
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    setenv("SDL_VIDEODRIVER", "dummy", 1);
  }

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    log_error("SDL_Init(VIDEO) failed: %s", SDL_GetError());
    /* 再退一步：强制 dummy 重试一次，保证无 X 环境也能跑 */
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
      log_error("SDL_Init(VIDEO, dummy) 仍失败: %s", SDL_GetError());
      return -1;
    }
  }
  if (TTF_Init() != 0) {
    log_error("TTF_Init failed: %s", TTF_GetError());
    return -1;
  }

  char window_title[160];
  snprintf(window_title, sizeof(window_title), "%.100s (zm_emu)",
           g_header.AppName[0] ? g_header.AppName : "applet");

  g_win =
      SDL_CreateWindow(window_title, SDL_WINDOWPOS_UNDEFINED,
                       SDL_WINDOWPOS_UNDEFINED, g_w, g_h, SDL_WINDOW_RESIZABLE);
  if (!g_win) {
    log_error("SDL_CreateWindow failed: %s", SDL_GetError());
    return -1;
  }

  /* 无头模式直接用软件渲染，避免驱动探测开销 */
  if (!g_headless)
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
  if (!g_ren)
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
  if (!g_ren) {
    log_error("SDL_CreateRenderer failed: %s", SDL_GetError());
    return -1;
  }

  SDL_RenderSetLogicalSize(g_ren, g_w, g_h);
  g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, g_w, g_h);
  if (!g_tex) {
    log_error("SDL_CreateTexture failed: %s", SDL_GetError());
    return -1;
  }
  g_frame = calloc((size_t)g_w * (size_t)g_h, 4);
  if (!g_frame) {
    log_error("分配合成帧缓冲失败");
    return -1;
  }

  /* 只登记屏幕尺寸；真正的图层缓冲要等 Unicorn 起来后才能分配，
   * 因此 0 号底板在第一次绘制（zm_layer_active）时惰性创建。 */
  zm_layer_reset(g_w, g_h);

  g_ready = true;
  g_draw_ops = 0;

  log_info("zm_gfx_init: %dx%d 画布已创建（%s，渲染后端=%s）", g_w, g_h,
           g_headless ? "无头" : "有窗口", SDL_GetCurrentVideoDriver());
  return 0;
}

bool zm_gfx_ready(void) { return g_ready; }

void zm_gfx_shutdown(void) {
  if (g_font) {
    TTF_CloseFont(g_font);
    g_font = NULL;
  }
  if (g_tex) {
    SDL_DestroyTexture(g_tex);
    g_tex = NULL;
  }
  free(g_frame);
  g_frame = NULL;
  if (g_ren) {
    SDL_DestroyRenderer(g_ren);
    g_ren = NULL;
  }
  if (g_win) {
    SDL_DestroyWindow(g_win);
    g_win = NULL;
  }
  g_ready = false;
  if (TTF_WasInit())
    TTF_Quit();
  SDL_Quit();
}

/* 把所有图层合成到 g_frame */
static void compose(void) {
  if (!g_ready || !g_frame)
    return;
  zm_layer_composite(g_uc, g_frame, g_w, g_h);
}

void zm_gfx_present(void) {
  if (!g_ready)
    return;
  compose();
  SDL_UpdateTexture(g_tex, NULL, g_frame, g_w * 4);
  SDL_RenderClear(g_ren);
  SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
  SDL_RenderPresent(g_ren);
  pump_events();
}

int zm_gfx_save_bmp(const char *path) {
  if (!g_ready)
    return -1;
  compose();
  SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
      g_frame, g_w, g_h, 32, g_w * 4, SDL_PIXELFORMAT_ARGB8888);
  int rc = -1;
  if (surf) {
    rc = SDL_SaveBMP(surf, path);
    if (rc != 0)
      log_warn("SDL_SaveBMP(%s) failed: %s", path, SDL_GetError());
    SDL_FreeSurface(surf);
  }
  return rc == 0 ? 0 : -1;
}

int zm_gfx_canvas_stats(uint32_t *out_nonzero_px,
                        uint32_t *out_distinct_colors) {
  if (out_nonzero_px)
    *out_nonzero_px = 0;
  if (out_distinct_colors)
    *out_distinct_colors = 0;
  if (!g_ready)
    return -1;

  compose();

  /* 用 8192 槽的开放寻址哈希粗略统计去重颜色数，足够判定"画面不是纯色" */
  enum { NSLOT = 8192 };
  uint32_t *slots = calloc(NSLOT, sizeof(uint32_t));
  uint8_t *used = calloc(NSLOT, 1);
  uint32_t distinct = 0, nonzero = 0;
  size_t total = (size_t)g_w * (size_t)g_h;

  for (size_t i = 0; i < total; i++) {
    uint32_t c = g_frame[i] | 0xFF000000u; /* 忽略 alpha 差异 */
    if ((c & 0x00FFFFFFu) != 0)
      nonzero++;
    if (!slots || !used || distinct >= NSLOT / 2)
      continue;
    uint32_t hsh = (c * 2654435761u) % NSLOT;
    while (used[hsh] && slots[hsh] != c)
      hsh = (hsh + 1) % NSLOT;
    if (!used[hsh]) {
      used[hsh] = 1;
      slots[hsh] = c;
      distinct++;
    }
  }

  free(slots);
  free(used);

  if (out_nonzero_px)
    *out_nonzero_px = nonzero;
  if (out_distinct_colors)
    *out_distinct_colors = distinct;
  return 0;
}

uint32_t zm_gfx_draw_ops(void) { return g_draw_ops; }

void zm_gfx_hold(uint32_t timeout_ms) {
  if (!g_ready || g_headless)
    return;
  zm_gfx_present();
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

bool zm_gfx_event_loop(void (*on_click)(uint32_t x, uint32_t y),
                       uint32_t timeout_ms) {
  if (!g_ready)
    return false;

  /* present 最终画布，让用户看到 applet 绘制的界面 */
  zm_gfx_present();
  if (g_stop_requested)
    return false;

  int win_w = g_w, win_h = g_h;
  SDL_GetWindowSize(g_win, &win_w, &win_h);

  SDL_Event e;
  Uint32 start = SDL_GetTicks();
  for (;;) {
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        g_stop_requested = 1;
        return false; /* 用户关窗 → 模拟结束 */
      }
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        g_stop_requested = 1;
        return false;
      }
      if (e.type == SDL_MOUSEBUTTONDOWN && on_click) {
        uint32_t cx = (win_w > 0) ? (uint32_t)(e.button.x * g_w / win_w)
                                  : (uint32_t)e.button.x;
        uint32_t cy = (win_h > 0) ? (uint32_t)(e.button.y * g_h / win_h)
                                  : (uint32_t)e.button.y;
        on_click(cx, cy);
        return true; /* 已派发点击事件 → 让模拟器执行 handler */
      }
    }
    if (timeout_ms != 0 && SDL_GetTicks() - start >= timeout_ms)
      return false; /* 超时 → 模拟结束 */
    SDL_Delay(16);
  }
}

/* ---------- gfx trap 处理函数 ---------- */

/* gfx.fillRect(rect*)：applet 在绘制末尾调用，语义疑似 invalidate。
 * 若真按 rect 用黑色填充会把整张画面刷掉，因此只当作"请求刷新"。 */
uint32_t zm_gfx_fillRect(uc_engine *uc, uint32_t rect_ptr) {
  (void)uc;
  (void)rect_ptr;
  return 0;
}

uint32_t zm_gfx_commit(uc_engine *uc) {
  (void)uc;
  zm_gfx_present();
  return 0;
}

uint32_t zm_gfx_drawText(uc_engine *uc, uint32_t rect_ptr, uint32_t text_ptr,
                         uint32_t text_len, uint32_t sp) {
  /* 先判定编码方式，因为字节扫描策略依赖编码 */
  bool utf16 = (strstr(g_app_pathname, "00000405") != NULL ||
                strstr(g_app_pathname, "00000102") != NULL);

  uint8_t raw[512];
  uint32_t n = 0;
  if (text_ptr) {
    /* r3 在 00000405 里是 ROOT[0xD8] get_tick 的返回值（递增 tick），
     * 不是文本长度——统一按固定上限读取。 */
    uint32_t cap = (uint32_t)sizeof(raw);
    if (uc_mem_read(uc, text_ptr, raw, cap) != UC_ERR_OK) {
      n = 0;
    } else if (utf16) {
      /* UTF-16LE 文本：以双零字节 00 00 作为终止符（单 00 是合法码元
       * 的低字节，如 '1' = 31 00）。 */
      while (n + 1 < cap && (raw[n] != 0 || raw[n + 1] != 0))
        n += 2;
    } else {
      while (n < cap && raw[n] != 0)
        n++;
    }
  }

  char text[512];
  uint32_t m = 0;
  /* 00000405 / 00000102 的文本经 ROOT[0x20] (zm_strcpy) 转为 UTF-16LE 后
   * 再传入 drawText。CJK 码元字节模式与 UTF-8 相似，启发式不可靠，故按
   * applet ID 门控：这些 applet 强制按 UTF-16LE 解码；其他保持原字节。 */
  if (utf16) {
    for (uint32_t i = 0; i + 1 < n && m + 4 < sizeof(text); i += 2) {
      uint32_t cp = (uint32_t)raw[i] | ((uint32_t)raw[i + 1] << 8);
      if (cp == 0)
        break;
      if (cp < 0x80) {
        text[m++] = (char)cp;
      } else if (cp < 0x800) {
        text[m++] = (char)(0xC0 | (cp >> 6));
        text[m++] = (char)(0x80 | (cp & 0x3F));
      } else {
        text[m++] = (char)(0xE0 | (cp >> 12));
        text[m++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        text[m++] = (char)(0x80 | (cp & 0x3F));
      }
    }
  } else {
    for (uint32_t i = 0; i < n && m + 1 < sizeof(text); i++)
      text[m++] = (char)raw[i];
  }
  text[m] = '\0';

  uint32_t color = uc_read32(uc, sp);
  uint32_t font_sz = uc_read32(uc, sp + 8);
  fb_draw_text(uc, rect_ptr, text, color, (int)font_sz);
  return 0;
}

uint32_t zm_gfx_drawRect(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                         uint32_t sp) {
  int h = (int)uc_read32(uc, sp);
  uint32_t color = uc_read32(uc, sp + 4);
  fb_draw_rect((int)x, (int)y, (int)w, h, color);
  return 0;
}

uint32_t zm_gfx_fillRect2(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                          uint32_t sp) {
  int h = (int)uc_read32(uc, sp);
  uint32_t color = uc_read32(uc, sp + 4);
  fb_fill_rect((int)x, (int)y, (int)w, h, color);
  return 0;
}

/* gfx.fillRect5C(x, y, w, h)：00000440 的 sub_31900 调用，无颜色参数
 * （颜色可能绑定在 gfx 对象内部）。暂用白色填充，保证可见。 */
uint32_t zm_gfx_fillRect5C(uc_engine *uc, uint32_t x, uint32_t y, uint32_t w,
                           uint32_t sp) {
  int h = (int)uc_read32(uc, sp);
  fb_fill_rect((int)x, (int)y, (int)w, h, 0xFFFFFFFF);
  return 0;
}

/* gfx.vtB4(obj, xy_ptr, imginfo_ptr, param_ptr)：三段式横条 blit（进度条）。
 *   R1 → 客户机 8 字节 {x, y}
 *   R2 → 客户机 8 字节 {w, h}（来自 getSize 的 imgInfo）
 *   R3 → 客户机 16 字节 {srcX, srcY, segW, color}
 * 00000001 的 sub_3B08 连续调 3 次画三段横条。 */
uint32_t zm_gfx_vtB4(uc_engine *uc, uint32_t obj, uint32_t xy_ptr,
                     uint32_t info_ptr, uint32_t param_ptr) {
  (void)obj;
  if (!xy_ptr || !info_ptr || !param_ptr)
    return 0;
  int32_t x = (int32_t)uc_read32(uc, xy_ptr);
  int32_t y = (int32_t)uc_read32(uc, xy_ptr + 4);
  uint32_t h = uc_read32(uc, info_ptr + 4);
  uint32_t srcX = uc_read32(uc, param_ptr);
  uint32_t srcY = uc_read32(uc, param_ptr + 4);
  uint32_t segW = uc_read32(uc, param_ptr + 8);
  uint32_t color = uc_read32(uc, param_ptr + 12);
  (void)srcY;
  /* 三段横条：从 (x+srcX, y) 起画 segW 宽、h 高的色块 */
  fb_fill_rect(x + (int32_t)srcX, y, (int32_t)segW, (int32_t)h, color);
  return 0;
}

/* GFX_VT[0x68] DrawLine(this, x1, y1, x2, y2, color)
 *   R1=x1 R2=y1 R3=x2 [sp]=y2 [sp+4]=color
 * 00000405 sub_8081C 用 4 次调用画内容矩形（instance+0xD8/DC/E0/E4）的四条
 * 1px 边框。水平/垂直直接用图层填充；斜线用 Bresenham 逐点写。 */
uint32_t zm_gfx_draw_line(uc_engine *uc, uint32_t x1, uint32_t y1,
                          uint32_t x2, uint32_t sp) {
  int32_t y2 = (int32_t)uc_read32(uc, sp);
  uint32_t color = uc_read32(uc, sp + 4);
  ZmLayer *L = zm_layer_active();
  if (!L)
    return 0;
  int32_t X1 = (int32_t)x1, Y1 = (int32_t)y1, X2 = (int32_t)x2, Y2 = y2;

  if (Y1 == Y2) { /* 水平线 */
    int32_t sx = X1 < X2 ? X1 : X2, ex = X1 < X2 ? X2 : X1;
    zm_layer_fill(uc, L, sx, Y1, ex - sx + 1, 1, color);
    return 0;
  }
  if (X1 == X2) { /* 垂直线 */
    int32_t sy = Y1 < Y2 ? Y1 : Y2, ey = Y1 < Y2 ? Y2 : Y1;
    zm_layer_fill(uc, L, X1, sy, 1, ey - sy + 1, color);
    return 0;
  }

  /* Bresenham 斜线 */
  int32_t dx = X2 > X1 ? X2 - X1 : X1 - X2;
  int32_t dy = Y2 > Y1 ? Y2 - Y1 : Y1 - Y2;
  int32_t sx = X1 < X2 ? 1 : -1;
  int32_t sy = Y1 < Y2 ? 1 : -1;
  int32_t err = dx - dy;
  int32_t x = X1, y = Y1;
  for (int guard = 0; guard < 8192; guard++) {
    zm_layer_fill(uc, L, x, y, 1, 1, color);
    if (x == X2 && y == Y2)
      break;
    int32_t e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x += sx;
    }
    if (e2 < dx) {
      err += dx;
      y += sy;
    }
  }
  return 0;
}

/* GFX_VT[0x44]：00000405 sub_8081C 调用，无参 display 操作，no-op 返 0 */
uint32_t zm_gfx_vt44(uc_engine *uc) {
  (void)uc;
  return 0;
}

uint32_t zm_gfx_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                     uint32_t r2, uint32_t r3) {
  (void)uc;
  log_debug("stub gfx[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1, r2,
            r3);
  return 0;
}

/* GFX_VT[0x48]：返回屏幕宽度 */
uint32_t zm_gfx_get_width(uc_engine *uc) {
  (void)uc;
  return (uint32_t)(g_w > 0 ? g_w : (int)g_header.ScreenW);
}

/* GFX_VT[0x4C]：measureChar(gfx, char_ptr, count, width_out)
 * 用当前字体做真实字宽测量，避免文本全叠在一起。 */
uint32_t zm_gfx_measure_char(uc_engine *uc, uint32_t gfx, uint32_t char_ptr,
                             uint32_t count, uint32_t width_out) {
  (void)gfx;
  uint32_t width = 8;

  if (char_ptr) {
    uint16_t ch = 0;
    if (uc_mem_read(uc, char_ptr, &ch, 2) == UC_ERR_OK && ch != 0) {
      TTF_Font *f = get_font(g_font_size ? g_font_size : 14);
      int minx, maxx, miny, maxy, adv;
      if (f && TTF_GlyphMetrics(f, ch, &minx, &maxx, &miny, &maxy, &adv) == 0 &&
          adv > 0) {
        width = (uint32_t)adv;
      }
    }
  }
  if (count > 1)
    width *= count;

  if (width_out)
    uc_write32(uc, width_out, width);
  return 0;
}

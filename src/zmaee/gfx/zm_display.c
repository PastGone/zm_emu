#include "zm_display.h"

#include "../../emu.h"
#include "../../event.h"
#include "../../log/log.h"
#include "../../trap.h" /* getArg：第 5 个及以后的参数 */
#include "../../tool/uc_helper.h"
#include "../runtime/timer/zm_timer.h" /* zm_timer_poll：到期定时器检查 */
#include "../audio/zm_audio.h" /* zm_media_pending_cb_poll：音频完成回调跳板 */
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
 *   - 实测过行为的槽（SetActiveLayer/UpdateEx/SelectFont/GetFontHeight/
 *     MeasureString/DrawText/DrawRect/FillRect/Update/Refresh）按真实行为实现；
 *   - 返回对象的（CreateBitmap/LoadBitmap）返回 BITMAP 单例；
 *   - 其余接 zm_display_stub：仅记录日志、返回 0。
 *
 * 注意：IDisplay 方法 r0 = this（display 对象），真实绘制参数从 r1 起；
 * 未实测槽的参数布局待后续 RE 校准。
 * ========================================================================= */

/* ---------- 渲染后端状态 ---------- */

/* ---------- 字体选择 ----------
 * 真机的文本渲染不在固件里：ZMAEE_IDisplay_DrawText 把 UCS-2 转 UTF-8 后走
 * Android（JNI NewStringUTF + AndroidAEE_GetTextBitmap），字形由**系统字体**
 * 提供，天然含 CJK。宿主侧用 SDL_ttf 顶替，就得自己挑一个含汉字的字体：
 * 早期写死的 LiberationSans 只有拉丁字形，汉字全落 .notdef → 豆腐块。
 * 优先级：ZM_FONT 环境变量 > 常见 CJK 字体 > 拉丁兜底。 */
#define ZM_FONT_PATH "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"
static const char *g_font_cands[] = {
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Light.ttc",
    ZM_FONT_PATH, /* 拉丁兜底：没有 CJK 字体时保持旧行为 */
};
static const char *pick_font_path(void) {
  static const char *chosen = NULL;
  if (chosen)
    return chosen;
  const char *env = getenv("ZM_FONT");
  if (env && env[0]) {
    chosen = env;
  } else {
    chosen = ZM_FONT_PATH;
    for (size_t i = 0; i < sizeof(g_font_cands) / sizeof(g_font_cands[0]); i++) {
      FILE *f = fopen(g_font_cands[i], "rb");
      if (f) {
        fclose(f);
        chosen = g_font_cands[i];
        break;
      }
    }
  }
  log_info("文本字体文件: %s", chosen);
  return chosen;
}

/* 在 .ttc 里挑 face —— Noto Sans CJK 这类集合体里通常有
 * JP / KR / SC / TC / HK 五个 face，而 TTF_OpenFont 只会开 **face 0 = JP**，
 * 简体汉字于是带上日文写法（直/骨/画/边…）。
 * 这里按 family 名扫一遍，取第一个含 "SC"（简体）的 face；没找到就回退 0。
 * 结果按文件路径缓存，字号变化不会重复扫。 */
static int pick_font_face(const char *path, int ptsize) {
  static const char *cached_path = NULL;
  static int cached_idx = 0;
  if (cached_path == path)
    return cached_idx;
  cached_path = path;
  cached_idx = 0;
  for (int idx = 0; idx < 16; idx++) {
    TTF_Font *f = TTF_OpenFontIndex(path, ptsize, idx);
    if (!f)
      break; /* 没有更多 face 了 */
    const char *fam = TTF_FontFaceFamilyName(f);
    if (fam && strstr(fam, "SC")) {
      cached_idx = idx;
      TTF_CloseFont(f);
      break;
    }
    TTF_CloseFont(f);
  }
  return cached_idx;
}

static SDL_Window *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static SDL_Texture *g_canvas = NULL; /* ARGB8888 RenderTarget，作为画布 */
static int g_w = 0;
static int g_h = 0;

/* 字体按 font_size 缓存，size 变化时重新打开 */
static TTF_Font *g_font = NULL;
static int g_font_size = 0;
static uint32_t g_sel_font = 0; /* 当前选中字体索引（SelectFont 写入） */

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
  if (font_size <= 0) {
    /* 真机的字号来自字体上下文（dword_64BA8 +0x3C/0x40/0x44，按 SelectFont
     * 选中的字体类型取），模拟器没有那份上下文，给 240x320 上的合理默认值，
     * 并允许 ZM_FONT_SIZE 覆盖。 */
    const char *fs = getenv("ZM_FONT_SIZE");
    font_size = (fs && *fs) ? atoi(fs) : 14;
  }
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
  const char *fpath = pick_font_path();
  int face = pick_font_face(fpath, font_size);
  g_font = TTF_OpenFontIndex(fpath, font_size, face);
  if (!g_font) {
    log_error("TTF_OpenFontIndex(%s, face=%d) failed: %s", fpath, face,
              TTF_GetError());
    return NULL;
  }
  g_font_size = font_size;
  {
    const char *fam = TTF_FontFaceFamilyName(g_font);
    log_info("文本字体: %s face=%d (%s) %dpx", fpath, face, fam ? fam : "?", font_size);
  }
  return g_font;
}

/* ---------- 文本编码：UCS-2(LE) → UTF-8 ----------
 * RE（ZMAEE_Ucs2_2_Utf8(a1=src, a2=字数, a3=dst, a4=dst容量)）：
 *   逐 16 位字符写 1/2/3 字节 UTF-8；**写下一个字符前若容量不够就停**
 *   （`if (a4 <= v7 + n) break;`），最后一定补 '\0'，返回写入字节数。
 * 真机的 IDisplay::MeasureString / DrawText 在对字符串做任何处理前都会先调它
 * （把 UCS-2 转成给 Android NewStringUTF 用的 UTF-8）。
 *
 * 【2026-09 修正】模拟器以前跳过了这一步：把 UCS-2 的**原始字节**当 UTF-8
 * 直接喂 TTF_RenderUTF8_Blended。汉字两字节 "CD 64"（U+64CD 操）按 UTF-8 解
 * 是一个非法首字节 + 一个 ASCII，SDL_ttf 把非法字节替成 U+FFFD，落到字体里
 * 就是 .notdef —— 屏幕上看到的"豆腐块"。现在按真机先转换。
 * 返回值 = 产出的 UTF-8 字节数（不含结尾 '\0'）。 */
static size_t ucs2_to_utf8(uc_engine *uc, uint32_t ptr, uint32_t max_chars,
                           char *out, size_t cap) {
  size_t o = 0;
  if (!ptr || !max_chars || cap == 0) {
    if (cap)
      out[0] = '\0';
    return 0;
  }
  for (uint32_t i = 0; i < max_chars; i++) {
    uint8_t b[2];
    if (uc_mem_read(uc, ptr + i * 2u, b, 2) != UC_ERR_OK)
      break;
    uint16_t c = (uint16_t)(b[0] | (b[1] << 8));
    if (c == 0)
      break; /* 真机：遇 UCS-2 '\0' 即有效长度到此为止 */
    size_t need = (c < 0x80) ? 1u : ((c < 0x800) ? 2u : 3u);
    if (cap <= o + need)
      break; /* 真机：容量不够就停 */
    if (need == 1) {
      out[o++] = (char)c;
    } else if (need == 2) {
      out[o++] = (char)(0xC0 | (c >> 6));
      out[o++] = (char)(0x80 | (c & 0x3F));
    } else {
      out[o++] = (char)(0xE0 | (c >> 12));
      out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[o] = '\0';
  return o;
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

/* 透明判定已收敛为 RE 语义（ZMAEE 的透明是「透明色」而非 alpha 通道）：
 *   - 层载荷 +0x2C 非 0 → 走 mask（该层像素 == +0x30 转 RGB565 则跳过）
 *   - 层载荷 +0x2C == 0 → 走 copy（整块直写，不透明）
 *   - 位图对象 +20        → IBitmap 自己的透明色（见 zm_bitmap_SetTransColor）
 * 以前这里是 6 个全局开关（ZM_TRANS_KEY / ZM_FB_MASK0 / ZM_NO_SKIP …）与
 * "全局透明色回退"，语义互相矛盾；现在全部删除，只按层字段走。 */
/* ZM_COLORSTAT=1：统计层缓冲 RGB565 颜色直方图（诊断透明色是否被写入） */
static bool g_colorstat = false;

/* argb8888 → rgb565（写客户机层缓冲用） */
static inline uint16_t to_rgb565(uint32_t argb) {
  unsigned r = (argb >> 16) & 0xFFu, g = (argb >> 8) & 0xFFu,
           b = argb & 0xFFu;
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline void fb_px(int x, int y, uint32_t argb);

/* ---- 当前绘制目标 = 活动层的像素缓冲 ----
 * RE：绘制类槽的目标都是 `&display[52*活动层 + 36]`（载荷），像素缓冲在
 * 载荷 +0x24。活动层变化时刷新一次缓存，避免逐像素去读客户机内存。 */
static uint32_t g_draw_buf = 0;
static uint32_t g_draw_w = 0, g_draw_h = 0;
static uint32_t g_draw_act = 0xFFFFFFFFu;

static void fb_refresh_draw_target(void) {
  if (!g_uc)
    return;
  uint32_t act = uc_read32(g_uc, DISPLAY + 8);
  if (act == g_draw_act && g_draw_buf)
    return;
  g_draw_act = act;
  zm_layer_t L;
  if (zm_layer_get(g_uc, DISPLAY, act, &L) == 0 && L.buf && L.w && L.h) {
    g_draw_buf = L.buf;
    g_draw_w = L.w;
    g_draw_h = L.h;
  } else {
    g_draw_buf = 0;
    g_draw_w = g_draw_h = 0;
  }
}

static inline void fb_px(int x, int y, uint32_t argb) {
  if ((unsigned)x >= (unsigned)g_fb_w || (unsigned)y >= (unsigned)g_fb_h)
    return;
  if (g_fb)
    g_fb[(size_t)y * (size_t)g_fb_w + (size_t)x] = argb;

  /* 同步写客户机可见的**活动层像素缓冲**（RGB565）。
   *
   * 【2026-09 修正】以前这里写死 `LAYER_BUF` —— 一个我们自己在内存布局里
   * 挑的常量地址。后果：所有经 IDisplay 绘制槽进来的图形都落在"没有层
   * 拥有的那块内存"上，而 applet 上屏用的是层载荷里的缓冲（见 UpdateEx），
   * 于是画面永远对不上。
   * RE：绘制类槽（FillRect / DrawBitmap / BitBlt …）的目标都是
   *     `&display[52*活动层 + 36]` 的 +0x24 像素缓冲；FillRect 早已如此实现。
   * 现在统一走 g_draw_buf（由 fb_refresh_draw_target 随活动层刷新）。 */
  if (g_uc && g_draw_buf && (unsigned)x < (unsigned)g_draw_w &&
      (unsigned)y < (unsigned)g_draw_h) {
    uint16_t c = to_rgb565(argb);
    uc_mem_write(g_uc,
                 g_draw_buf + ((uint32_t)y * g_draw_w + (uint32_t)x) * 2u, &c,
                 sizeof(c));
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

/* 文本缓冲 → UTF-8（自动判定 UCS-2 还是窄串）
 *
 * 真机的 IDisplay::DrawText / MeasureString **只吃 UCS-2**（内部先
 * Ucs2_2_Utf8 再交 Android 渲染）。但实测 applet 侧存在两种用法：
 *
 *   00000506（帮助页）: 逐字符 `STRH R6,[SP,#var_24]` 写 UCS-2
 *                       （"操"=0x64CD），len = **字符数**；
 *   00000102（数字键盘）: 窄串 + len = **字节数**
 *                       （`sprintf("%u")` → `str_copy` → `DrawText`，
 *                        实测缓冲为 31 30 00 00 = "10"）。
 *
 * 后者若一律按 UCS-2 解读，`31 30` 会读成一个 16 位字 U+3031，
 * 症状正是"1~9 正常、10 以后全变乱码汉字"（截图实测）。
 *
 * 判据：len 个字节**全部落在 0x01..0x7F**（既无 NUL 也无高位字节）→ 窄串；
 * 否则按 UCS-2 转。可判定性说明：
 *   - UCS-2 的 ASCII 文本必然出现 0x00 高位字节（"AB" = 41 00 42 00）→ UCS-2 ✓
 *   - 汉字的 UTF-16 高位字节 ≥ 0x4E（"操"= CD 64）→ UCS-2 ✓
 *   - 纯 ASCII 单字符（"1" = 31 00）两种解读结果相同，不影响 ✓
 * 拿不准时一律走真机语义（UCS-2），所以这个适配是"只加法"。 */
static size_t text_to_utf8(uc_engine *uc, uint32_t ptr, uint32_t len, char *out,
                           size_t cap) {
  if (cap == 0)
    return 0;
  out[0] = '\0';
  if (!ptr || !len)
    return 0;

  /* 单字符不判窄串：1 个 UCS-2 字（2 字节）与 1 个窄字节在法律上不可区分，
   * 而真机语义是 UCS-2。这里必须走 UCS-2 —— 否则低字节恰好是可打印 ASCII 的
   * 汉字会被"腰斩"（实测 506 帮助页："作"=5C 4F 渲染成 '\'、"键"=2E 95 渲染
   * 成 '.'、"按"=09 63 渲染成制表符）。反过来 0x0031 与窄串 "1" 结果相同，
   * 所以单字符走 UCS-2 对窄串样本也无损。 */
  if (len >= 2) {
    uint32_t i = 0;
    for (; i < len && i < 512u; i++) {
      uint8_t c = 0;
      if (uc_mem_read(uc, ptr + i, &c, 1) != UC_ERR_OK)
        break;
      /* 只有**可打印 ASCII**（0x20..0x7E）才算窄串；控制字符与高位字节
       * 一律按 UCS-2 解（汉字的高位字节 ≥ 0x4E，控制字符则是 UCS-2 的低
       * 字节，两种都不该被当成窄串）。 */
      if (c < 0x20 || c >= 0x7F)
        break;
    }
    if (i == len) { /* 全是可打印 ASCII → 窄串，原样输出 */
      size_t n = len < cap - 1 ? len : cap - 1;
      for (size_t k = 0; k < n; k++) {
        uint8_t c = 0;
        uc_mem_read(uc, ptr + (uint32_t)k, &c, 1);
        out[k] = (char)c;
      }
      out[n] = '\0';
      return n;
    }
  }
  return ucs2_to_utf8(uc, ptr, len, out, cap);
}

/* 在 rect_ptr 指向的矩形 {x,y,w,h} 内按对齐标志绘制文本。
 * 用 TTF 光栅化成 ARGB8888 表面后逐像素 alpha 混合进软件帧缓冲
 * （不再走 SDL_Render*，保证与 BitBlt 等操作共用同一块画布）。
 *
 * text 必须是**已转好的 UTF-8**（真机在这里之前先过 Ucs2_2_Utf8，见
 * ucs2_to_utf8）。
 * flags = 真机 DrawText 的 a7（栈 +8）：
 *   &2 右对齐   &4 水平居中   &0x20 底对齐   &0x10 垂直居中
 * 其余位（实测 applet 常带 bit0）含义未知，忽略 —— 它不参与定位。 */
static void fb_draw_text(uc_engine *uc, uint32_t rect_ptr, const char *text,
                         uint32_t color, int font_size, uint32_t flags) {
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
  /* 对齐：忠实照搬真机的位判断（真机先用 MeasureString 量宽高再按 a7 定位） */
  int dx = rx, dy = ry;
  if (flags & 2)
    dx = rx + rw - tw; /* 右对齐 */
  else if (flags & 4)
    dx = rx + (rw - tw) / 2; /* 水平居中 */
  if (flags & 0x20)
    dy = ry + rh - th; /* 底对齐 */
  else if (flags & 0x10)
    dy = ry + (rh - th) / 2; /* 垂直居中 */

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
 * mode：显示接口的"类型字节"，即固件 ZMAEE_GDI_BitBlt_Ext 的 a6，
 * 它用来选 `g_zmaee_blt_func[mode]`（那族函数正好 8 个：
 * Blt / Blt_Mir / Blt_Mir90 / Blt_Mir180 / Blt_Mir270 /
 * Blt_Rot90 / Blt_Rot180 / Blt_Rot270 = 4 镜像 + 3 旋转 + 恒等，D4 群）。
 *
 * 【2026-09 修正】旧表（1=垂直翻转,2=水平翻转,3=180°,4=转置,
 * 5=转置+垂直,6=转置+水平,7=转置+180°）是**猜的**，实际 1/2/3/6/7 五项错位。
 * 正确表由三方交叉验证得出：
 *   ① applet 自带的 8 项变换表（00000506 sub_5070 → sub_50D8/5108/5138/
 *      5168/5180/51A0/51C0）逐函数的点映射；
 *   ② applet 用 byte_1F714（紧跟 "zms2" 字符串之后的 64 字节 D4 复合表）
 *      把原始 mode 换成变换索引 v13，再把 byte_1F714[v13] 当 mode 传给我们
 *      —— 其第 0 行 [0,3,1,6,4,5,7,2] 就是"索引 → mode"的对应；③ 固件函数命名。
 *   实测症状：net.zmspx（35x35 渔网）走 mode 5/7，mode7 被当成"反对角镜像"
 *   而不是"270° 旋转"，方形精灵就表现为**象限转置**。
 *
 * 现在（点映射，(X,Y) 为源像素局部坐标）：
 *   0 = 恒等 (X,Y)          1 = 左右翻转 (-X,Y)     2 = 反对角镜像 (-Y,-X)
 *   3 = 上下翻转 (X,-Y)     4 = 主对角镜像/转置 (Y,X) 5 = 90° (-Y,X)
 *   6 = 180° (-X,-Y)        7 = 270° (Y,-X)
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
      case 1: dx = w - 1 - sx; break;                              /* (-X,Y) */
      case 2: dx = h - 1 - sy; dy = w - 1 - sx; break;              /* (-Y,-X) */
      case 3: dy = h - 1 - sy; break;                              /* (X,-Y) */
      case 4: dx = sy; dy = sx; break;                             /* (Y,X) */
      case 5: dx = h - 1 - sy; dy = sx; break;                     /* (-Y,X) */
      case 6: dx = w - 1 - sx; dy = h - 1 - sy; break;              /* (-X,-Y) */
      case 7: dx = sy; dy = w - 1 - sx; break;                     /* (Y,-X) */
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
  fb_merge_layer(); /* 帧缓冲 → 宿主 g_fb（纯转换，不合成） */
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

/* ---- 诊断：重建 applet 的每帧调用序列 ----
 * 目的：搞清"applet 到底把画面画在哪一层、为什么上屏要层 0"。
 * 把 SetActiveLayer / FillRect / UpdateEx / StretchBlt 的关键参数按时间记进
 * 一个环形缓冲，在头几次 UpdateEx（= 上屏）时把最近若干条打出来。 */
#define ZM_SEQ_N 24
typedef struct {
  uint8_t kind; /* 1=SetActiveLayer 2=FillRect 3=UpdateEx 4=StretchBlt */
  uint32_t a0, a1, act, lr;
} zm_seq_ev;
static zm_seq_ev g_seq[ZM_SEQ_N];
static int g_seq_i = 0;
static int g_seq_dumped = 0;
static void zm_seq_push(uint8_t kind, uint32_t a0, uint32_t a1) {
  uint32_t lr = 0;
  if (g_uc)
    uc_reg_read(g_uc, UC_ARM_REG_LR, &lr);
  g_seq[g_seq_i].kind = kind;
  g_seq[g_seq_i].a0 = a0;
  g_seq[g_seq_i].a1 = a1;
  g_seq[g_seq_i].act = g_uc ? uc_read32(g_uc, DISPLAY + 8) : 0;
  g_seq[g_seq_i].lr = lr;
  g_seq_i = (g_seq_i + 1) % ZM_SEQ_N;
}
static void zm_seq_dump(void) {
  if (g_seq_dumped >= 3)
    return;
  g_seq_dumped++;
  log_info("[序列] 最近 %d 次调用（旧→新，act=当时活动层）：", ZM_SEQ_N);
  for (int k = 0; k < ZM_SEQ_N; k++) {
    int i = (g_seq_i + k) % ZM_SEQ_N;
    const zm_seq_ev *e = &g_seq[i];
    const char *nm = e->kind == 1   ? "SetActiveLayer"
                     : e->kind == 2 ? "FillRect"
                     : e->kind == 3 ? "UpdateEx"
                     : e->kind == 4 ? "StretchBlt"
                                    : "?";
    log_info("   %-15s a0=%-6u a1=0x%-8X act=%u lr=0x%X", nm, e->a0, e->a1,
             e->act, e->lr);
  }
}

/* 层 → 帧缓冲（= 固件的 sub_285D8 + ZMAEE_Blt）。
 *
 * RE 规格：
 *   ZMAEE_IDisplay_UpdateEx @00029C84 → sub_285D8 @000285D8
 *                                     → ZMAEE_Blt     @0002C7D0
 *   1) 遍历 applet 给的 layerList，**顺序即叠加顺序**（后者压前者）；
 *      idx > 0xF 跳过（无符号比较，idx=0 合法）；层无像素缓冲跳过。
 *   2) 每层整块贴，目标位置 = 层.x − 帧缓冲.x、层.y − 帧缓冲.y。
 *      注意 sub_285D8 **不读层的裁剪区**（+0x14..+0x20）——层裁剪只约束
 *      绘制路径（FillRect / StretchBlt），不约束合成。
 *   3) 裁剪发生在 Blt 内部：源矩形与**帧缓冲描述符的裁剪矩形**（desc[5..8]）
 *      求交。这里的 rx/ry/rw/rh 就是 UpdateEx 裁剪后的 rect。
 *   4) 透明只有一条判据：层载荷 +0x2C 非 0 → mask（像素 == +0x30 转 RGB565
 *      则跳过），== 0 → copy（整块直写）。没有"全局透明色"这种东西。
 *
 * list/count 为空表示不合成。 */
static void fb_composite(uc_engine *uc, int rx, int ry, int rw, int rh,
                         const uint32_t *list, uint32_t count) {
  if (!uc || !list || count == 0)
    return;
  static uint16_t row[LAYER_MAX_W];
  static uint16_t frow[LAYER_MAX_W];
  static uint32_t row32[LAYER_MAX_W]; /* 4 字节/像素层的原始行 */
  int fb_w = (int)LAYER_W, fb_h = (int)LAYER_H;
  if (fb_w <= 0 || fb_h <= 0)
    return;
  /* 帧缓冲的裁剪矩形（RE：desc[5..8]，由 UpdateEx 从 rect 填入） */
  int cx0 = rx > 0 ? rx : 0;
  int cy0 = ry > 0 ? ry : 0;
  int cx1 = rx + rw;
  int cy1 = ry + rh;
  if (cx1 > fb_w)
    cx1 = fb_w;
  if (cy1 > fb_h)
    cy1 = fb_h;
  if (cx0 >= cx1 || cy0 >= cy1)
    return;
  for (uint32_t step = 0; step < count; step++) {
    uint32_t li = list[step];
    if (li > 0xFu)
      continue; /* RE：idx > 0xF 跳过 */
    zm_layer_t L;
    if (zm_layer_get(uc, DISPLAY, li, &L) != 0)
      continue; /* 该层不存在 / 无像素缓冲 */
    /* 按 ZMCF 决定每像素字节数（RE：.rodata:0x5B500 = {1,2,4,4,4}）。
     * 以前这里写死 `if (L.fmt != 1) continue;`，一旦层 0 的 fmt 因屏幕色深
     * 变化而不再是 1（16bpp→1、24bpp→2、32bpp→4，见 zm_display_base_depth），
     * 层 0 会被**静默跳过**，画面直接少一层且不报错。 */
    int bpp = zm_cf_bpp(L.fmt);
    if (bpp != 2 && bpp != 4) {
      static uint32_t warned = 0;
      if (warned < 4) {
        log_warn("[合成] 层%u 的 ZMCF=%u（%d 字节/像素）暂不支持，跳过", li,
                 L.fmt, bpp);
        warned++;
      }
      continue;
    }
    int pitch = (int)L.w;
    int lay_h = (int)L.h;
    if (pitch <= 0 || lay_h <= 0)
      continue;
    /* 层矩形 ∩ 帧缓冲裁剪矩形（RE：Blt 里的双边裁剪） */
    int lx1 = (int)L.x + pitch, ly1 = (int)L.y + lay_h;
    int x0 = ((int)L.x > cx0) ? (int)L.x : cx0;
    int y0 = ((int)L.y > cy0) ? (int)L.y : cy0;
    int x1 = (lx1 < cx1) ? lx1 : cx1;
    int y1 = (ly1 < cy1) ? ly1 : cy1;
    int lw = x1 - x0, lh = y1 - y0;
    if (lw <= 0 || lh <= 0)
      continue;
    int soff_x = x0 - (int)L.x; /* 层内源列偏移 */
    int soff_y = y0 - (int)L.y; /* 层内源行偏移 */
    int use_tc = (L.tenable != 0);
    uint16_t tc = use_tc ? to_rgb565(L.tcolor) : 0;
    for (int i = 0; i < lh; i++) {
      uint32_t rowoff = L.buf +
                        (uint32_t)(soff_y + i) * (uint32_t)pitch * (uint32_t)bpp +
                        (uint32_t)soff_x * (uint32_t)bpp;
      if (bpp == 2) {
        if (uc_mem_read(uc, rowoff, row, (size_t)lw * 2u) != UC_ERR_OK)
          break;
      } else {
        /* 4 字节/像素：整行读回后统一降到 RGB565 再合并，这样透明色比较与
         * 2 字节路径共用同一套语义。注意这条路径目前没有 applet 覆盖
         * （我们只跑 16bpp），属"按布局直译"，待有 32bpp 的 applet 再校准。 */
        if (uc_mem_read(uc, rowoff, row32, (size_t)lw * 4u) != UC_ERR_OK)
          break;
        for (int x = 0; x < lw; x++)
          row[x] = to_rgb565(row32[x]);
      }
      uint32_t fb_off = (uint32_t)(y0 + i) * (uint32_t)fb_w * 2u +
                        (uint32_t)x0 * 2u;
      if (use_tc) {
        /* mask：透明像素不动 → 保留下层 / 上一帧的内容（RE：Mask16To16） */
        if (uc_mem_read(uc, FRAMEBUF + fb_off, frow, (size_t)lw * 2u) !=
            UC_ERR_OK)
          break;
        for (int x = 0; x < lw; x++)
          if (row[x] != tc)
            frow[x] = row[x];
        uc_mem_write(uc, FRAMEBUF + fb_off, frow, (size_t)lw * 2u);
      } else {
        /* copy：整行直写，不透明（RE：Copy16To16） */
        uc_mem_write(uc, FRAMEBUF + fb_off, row, (size_t)lw * 2u);
      }
    }
  }
}

/* ---- "applet 说了算"的合成参数 ----
 * UpdateEx 每次调用都记下它的 rect 与层列表。宿主兜底 present 用最近一次
 * 记录（本 applet 实测恒为 rect={0,0,240,320}、list={0}），而不是自己猜。
 * 以前的"层 0 最后叠加"（ZM_LAYER_ORDER）启发式就是这条记录的手工近似。 */
#define ZM_LAYER_LIST_MAX 4
static uint32_t g_comp_list[ZM_LAYER_LIST_MAX];
static uint32_t g_comp_count = 0;
static int g_comp_x = 0, g_comp_y = 0, g_comp_w = 0, g_comp_h = 0;
static bool g_comp_valid = false;

/* 固件 ZMAEE_IDisplay_Update 的默认层列表在 .rodata:0x5B5D8（4 项），
 * 内容尚未取到；而这条路径本 applet 从不调用（槽位统计 +0x28 = 0 次），
 * 故暂以 {0,1,2,3} 占位 —— 待取到该表后替换为真实值。 */
static const uint32_t ZM_UPDATE_DEFAULT_LIST[4] = {0, 1, 2, 3};

/* 宿主兜底合成：applet 自绘且不调 Update（实测它调的是 UpdateEx），
 * 所以事件循环每 16ms 主动合成一次；参数优先取最近一次 UpdateEx 的记录。 */
static void fb_composite_default(void) {
  if (!g_uc)
    return;
  if (g_comp_valid)
    fb_composite(g_uc, g_comp_x, g_comp_y, g_comp_w, g_comp_h, g_comp_list,
                 g_comp_count);
  else
    fb_composite(g_uc, 0, 0, (int)LAYER_W, (int)LAYER_H, ZM_UPDATE_DEFAULT_LIST,
                 4);
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
    memset(hist, 0, sizeof(hist));
    memset(mhist, 0, sizeof(mhist));
  }
  int nmag = 0;
  int w = g_fb_w < LAYER_W ? g_fb_w : LAYER_W;
  int h = g_fb_h < LAYER_H ? g_fb_h : LAYER_H;

  /* 注意：这里**不再**做任何合成。层 → 帧缓冲由 fb_composite 负责，调用点是
   * UpdateEx / Update / Refresh（applet 驱动）与事件循环的兜底 tick。
   * 本函数只剩"帧缓冲 → 宿主 g_fb"的边界转换（guest RGB565 → ARGB8888，
   * 供 SDL 上传），不做透明跳过、不猜顺序。 */

  uint16_t row[LAYER_MAX_W];
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
    /* 品红家族 top-3：applet 的透明底通常就是这一族颜色 */
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
        log_info("  [探针]品红家族 #%d: 0x%04X × %u", k + 1, bestc, best);
        mhist[bestc] = 0;
      }
    }
    log_info("  [探针]品红家族像素 %d/帧", nmag / 60);
    /* 基础层（applet 用 GetBaseLayerBuffer 拿到的缓冲）里有没有内容？
     * 有 → applet 确实把背景画在基础层；空 → 它没用这个缓冲。 */
    {
      uint32_t ba = zm_display_base_layer_addr();
      if (ba) {
        static uint16_t brow[LAYER_MAX_W];
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

  /* ---- 绘制尺寸的唯一真源 = .app 头部的主屏尺寸 ----
   * g_layer_w/g_layer_h 由 main.c 解析头部后写入；LAYER_W/LAYER_H 就是它们。
   * 这里让 窗口 / 逻辑分辨率 / 软件帧缓冲 都跟随它，applet 拿到的
   * GetDeviceInfo 尺寸、层缓冲尺寸、窗口尺寸三者一致（呈现 1:1）。 */
  g_w = (int)LAYER_W;
  g_h = (int)LAYER_H;

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    log_error("SDL_Init failed: %s", SDL_GetError());
    return -1;
  }
  if (TTF_Init() != 0) {
    log_error("TTF_Init failed: %s", TTF_GetError());
    return -1;
  }
  /* 预热并记录"实际选中的字体 + face"：文本渲染出问题时（豆腐块/方框）
   * 第一件事就是看这行日志。 */
  (void)get_font(0);

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
    /* IMedia 的"上一首被打断"完成回调同理（真机在起新音乐前同步调，
     * 我们只能在 trap 里排队、到这里挂跳板） */
    if (zm_media_pending_cb_poll(g_uc))
      return true;

    /* 每轮兜底合成并呈现一次。
     *
     * 为什么需要：00000506 这类 applet 自带 GDI（ZMAEE_GDI_BitBlt_Ext 等），
     * 在定时器回调里**直接写层像素缓冲**，不走 IDisplay 的绘制 trap；它确实
     * 会调 UpdateEx（实测 170 次/8 秒），但两次调用之间有 ~47ms，中间若不
     * 主动合成，画面会在两次 UpdateEx 之间停滞。
     * 这里的合成参数（rect + 层列表）取自**最近一次 UpdateEx 的记录**，
     * 所以与 applet 自己的合成结果一致，不会来回闪。 */
    fb_composite_default();
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

/* ---- 屏幕色深 / 基础层深度（RE 已定案）----
 *
 * 真机链路：
 *   nativeAEEInit        → g_aee.screenDepth（= .bss:0x6480C）
 *   ZMAEE_IDisplay_Init  → 显示上下文 ctx+0x38（= .bss:0x64BE4）
 *   ZMAEE_IDisplay_GetBaseLayerDepth()（0002671C）：
 *       d = *(ctx+0x38);
 *       return ((unsigned)(d - 24) > 8) ? 1 : tbl_5B3E4[d - 24];
 *   表 .rodata:0x5B3E4（9 项）= {2,1,1,1,1,1,1,1,4}
 *   → 16bpp→1(RGB565)、24bpp→2、32bpp→4、其余→1
 *
 * 这个返回值被 ZMAEE_IDisplay_New 直接写进"层 0 载荷 +0x00"，而那一格正是
 * CreateLayer 的 fmt(a4∈[1,4]) —— 所以它就是 ZMCF 色格式枚举。
 * ZMCF → 每像素字节数见 zm_layer.h 的 ZM_CF_BPP（.rodata:0x5B500）。 */
#define ZM_SCREEN_DEPTH 16 /* 我们模拟的 g_aee.screenDepth */
static const uint32_t s_depth2zmcf[9] = {2, 1, 1, 1, 1, 1, 1, 1, 4};

int zm_display_screen_depth(void) { return ZM_SCREEN_DEPTH; }

uint32_t zm_display_base_depth(void) {
  unsigned r = (unsigned)(ZM_SCREEN_DEPTH - 24);
  if (r > 8u)
    return 1u;
  return s_depth2zmcf[r];
}

/* IDisplay 虚表 +0x18：RE 为 `ZMAEE_IDisplay_GetBaseLayerBuffer()`
 *   → 无参，返回显示上下文 ctx+0x04（.bss:0x64BB0）里的基础层缓冲指针。
 *
 * 【为什么这个很重要】
 * 层数组里 **层 0 是"基础层"**（RE 的铁证：`CreateLayer` 的 `(idx-1) > 0xE`
 * 拒绝 idx=0、`FreeAllLayer` 从 i=1 起循环永不释放层 0、`ZMAEE_IDisplay_New`
 * 内联构造层 0）。背景画在基础层里常驻，精灵画在层 1..15、每帧清空重画 ——
 * 合成时基础层打底，旧精灵自然被背景覆盖。
 *
 * 我们以前把 +0x18 当空 stub（返回 0），applet 拿到的缓冲指针是 0 → 背景
 * 无处可画 → 只能靠宿主帧缓冲"捡漏"留存 → 精灵移走后旧位置擦不掉（拖影）。
 *
 * 分配方式：真机在 ZMAEE_IDisplay_Init 里 `malloc(w*h*bpp)` 一次、终身不释放
 * （bpp = 色深==16 ? 2 : 4），随后 `memset(ptr, -1, size)` **整块 0xFF**。
 * 我们照做，但必须从像素池**尾部预留** —— 以前走 zm_pix_pool_alloc（会回绕
 * 复用的 bump 池），上千次 LoadBitmap 之后必然回绕把基础层冲掉。 */
static uint32_t g_base_layer = 0; /* 基础层缓冲的客户机地址（供诊断使用） */

uint32_t zm_display_GetBaseLayerBuffer(uc_engine *uc) {
  if (g_base_layer)
    return g_base_layer;
  int bpp = zm_cf_bpp(zm_display_base_depth());
  if (bpp <= 0)
    bpp = 2;
  uint32_t bytes = (uint32_t)LAYER_W * (uint32_t)LAYER_H * (uint32_t)bpp;
  uint32_t base = zm_pix_pool_reserve_tail(bytes);
  if (!base) {
    log_error("[基础层] 像素池尾部预留不足（需 %u 字节），基础层不可用", bytes);
    return 0;
  }
  g_base_layer = base;
  /* RE：Init 里 memset(ptr, -1, size) —— 初始为全 0xFF（RGB565 纯白），
   * 不是黑。 */
  {
    static uint8_t ff[4096];
    static bool ff_inited = false;
    if (!ff_inited) {
      memset(ff, 0xFF, sizeof(ff));
      ff_inited = true;
    }
    for (uint32_t off = 0; off < bytes; off += sizeof(ff)) {
      uint32_t n = bytes - off;
      if (n > sizeof(ff))
        n = sizeof(ff);
      uc_mem_write(uc, base + off, ff, n);
    }
  }
  log_info("[基础层] GetBaseLayerBuffer -> 0x%X (%dx%d ZMCF=%u = %d 字节/像素, "
           "共 %u 字节, 池尾预留)",
           base, LAYER_W, LAYER_H, zm_display_base_depth(), bpp, bytes);
  return base;
}

/* 基础层缓冲地址（0 = 未分配）。供 fb_merge_layer 的探针检查其内容。 */
uint32_t zm_display_base_layer_addr(void) { return g_base_layer; }

/* RE 为 `ZMAEE_IDisplay_GetBaseLayerDepth()`（0002671C，无参）。
 * 真机读的是显示上下文 ctx+0x38（= g_aee.screenDepth），不是 IDisplay 对象
 * 里的字段；我们把这个全局用 ZM_SCREEN_DEPTH 常量顶替（见上方说明）。 */
uint32_t zm_display_GetBaseLayerDepth(uc_engine *uc) {
  (void)uc;
  static uint32_t n = 0;
  if (n++ < 4)
    log_info("IDisplay.GetBaseLayerDepth -> %u（屏幕色深 %d，第 %u 次）",
             zm_display_base_depth(), ZM_SCREEN_DEPTH, n);
  return zm_display_base_depth();
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
   * 【已定案：16】以前恒返回 1（单层），导致 applet 只建一层、把背景和精灵
   * 全塞进同一层 —— 于是层里 83.7% 是透明色、精灵移走后旧位置擦不掉。
   *
   * 硬证据来自 ZMAEE_IDisplay_New（000282EC）的尾部：
   *     [IDisplay+0x364] = &IDisplay[0x368]
   * 而 IDisplay 的头部 36 字节 + 16 个层项 × 52 字节 = 868 = 0x364 ——
   * 层数组恰好结束在这个地址，所以对象里就是 **16 个层槽**。
   * 另有 CreateLayer 的 `(idx-1) > 0xE`、CreateLayerExt 的 `idx <= 0xF` 佐证。
   * 故不再需要 ZM_MAX_LAYER 逐个试。 */
  static uint32_t ncall = 0;
  if (ncall++ < 8)
    log_info("IDisplay.GetMaxLayerCount(第 %u 次) -> 16", ncall);
  return 16u;
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
/* IDisplay +0x14 = ZMAEE_IDisplay_FreeLayer(display, idx)
 *
 * 反编译（用户提供）：
 *   if (a1 == 0 || (unsigned)(a2 - 1) > 0xE) return -4;
 *   v5 = *(void **)(a1 + 52*a2 + 72);          // = 层载荷 +0x24（像素缓冲指针）
 *   if (v5) {
 *     if (*(BYTE*)(a1 + a2 + 20) != 0) free(v5);   // 该层"缓冲自持"才释放
 *     *(DWORD*)(a1 + 52*a2 + 72) = 0;              // 指针清零
 *   }
 *   return 0;
 *
 * 我们的像素池是 bump 分配器（zm_pix_pool_alloc，无 free、会回绕复用），
 * 所以"释放"只能记为日志；真正要紧的是**把客户机里的缓冲指针清零** ——
 * applet 自带 GDI 会拿它判断层是否有效。 */
uint32_t zm_display_FreeLayer(uc_engine *uc, uint32_t r0, uint32_t r1) {
  if (r0 == 0 || r1 < 1u || r1 > 15u)
    return (uint32_t)-4; /* (unsigned)(idx-1) > 0xE */
  uint32_t P = zm_layer_payload(r0, r1);
  uint32_t buf = uc_read32(uc, P + 0x24);
  if (!buf)
    return 0;
  uint8_t own = 0;
  uc_mem_read(uc, r0 + r1 + 20, &own, 1);
  uint32_t zero = 0;
  uc_mem_write(uc, P + 0x24, &zero, 4);
  log_info("FreeLayer(层=%u) 缓冲@0x%X（自持标志=%u%s）→ 指针已清零", r1, buf,
           own, own ? "，真机 free；本模拟器像素池为 bump 分配，仅释放记录" : "");
  return 0;
}

/* IDisplay +0x18 = ZMAEE_IDisplay_FreeAllLayer(display) */
uint32_t zm_display_FreeAllLayer(uc_engine *uc, uint32_t r0) {
  if (r0 == 0)
    return (uint32_t)-4;
  uint32_t zero = 0;
  uc_mem_write(uc, r0 + 8, &zero, 4); /* 活动层索引归 0 */
  int ret = 0;
  for (uint32_t i = 1; i != 16u; ++i) {
    if (zm_display_FreeLayer(uc, r0, i) != 0)
      ret = -1;
  }
  log_info("FreeAllLayer: 活动层归 0，层 1..15 已释放（层 0 基础层保留）→ %d", ret);
  return (uint32_t)ret;
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
/* UpdateEx 的真实实现（校验 + rect 裁剪 + 按 applet 给的列表合成）。
 * 供 Update（薄封装）与虚表槽共用。rect 参数按值传入，避免再写一趟客户机内存。 */
static uint32_t update_ex_impl(uc_engine *uc, uint32_t display, int32_t x, int32_t y,
                               int32_t w, int32_t h, const uint32_t *list,
                               uint32_t count) {
  /* 裁剪范围 = display+0x30/+0x34，也就是**层 0 载荷的 w/h**
   * （RE：ZMAEE_IDisplay_New 把屏宽/屏高写进层 0 的 +0x0C/+0x10）。 */
  int32_t sw = (int32_t)uc_read32(uc, display + 0x30);
  int32_t sh = (int32_t)uc_read32(uc, display + 0x34);
  if (sw <= 0)
    sw = (int32_t)LAYER_W;
  if (sh <= 0)
    sh = (int32_t)LAYER_H;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (x + w > sw)
    w = sw - x;
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > sh)
    h = sh - y;
  if (w <= 0 || h <= 0)
    return 0; /* RE：裁没了 → 成功但空操作 */

  /* 记下"applet 说了算"的参数，宿主兜底 tick 用它，而不是自己猜顺序 */
  g_comp_valid = true;
  g_comp_count = count < ZM_LAYER_LIST_MAX ? count : ZM_LAYER_LIST_MAX;
  if (g_comp_count)
    memcpy(g_comp_list, list, (size_t)g_comp_count * 4u);
  g_comp_x = (int)x;
  g_comp_y = (int)y;
  g_comp_w = (int)w;
  g_comp_h = (int)h;

  fb_composite(uc, (int)x, (int)y, (int)w, (int)h, list, count);
  /* 真机：UnLockFrameBuffer 内部会 AndroidAEE_Update 上屏 */
  fb_present();
  return 0;
}

/* +0x28：ZMAEE_IDisplay_Update(display, x, y, w, h)（RE 00029D88）
 * 就是个薄封装：把 (x,y,w,h) 组成局部 rect，然后
 *   UpdateEx(display, &rect, 4, unk_5B5D8)
 * 全部校验都在 UpdateEx 里，本函数没有任何判断。
 * 以前我们只接 r0（display），x/y/w/h 三个参数直接丢了。 */
uint32_t zm_display_Update(uc_engine *uc, uint32_t display, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h) {
  zm_display_slot_tick(0x28U);
  return update_ex_impl(uc, display, (int32_t)x, (int32_t)y, (int32_t)w,
                        (int32_t)h, ZM_UPDATE_DEFAULT_LIST, 4u);
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
/* +0x44：GetFontWidth(this)
 * 真机：sub_26378(this, &v2, nullptr)；sub_26378 写 *a2 = 选定字体尺寸/2 后因 a3==NULL
 * 直接返回 0，故 GetFontWidth 返回 v2/2 = (尺寸/2)/2 = 尺寸/4。context 空返回 -4。
 * 模拟器：选中字体尺寸即当前 g_font_size（≤0 默认 16），返回 尺寸/4。 */
uint32_t zm_display_GetFontWidth(uc_engine *uc, uint32_t r0, uint32_t r1) {
  zm_display_slot_tick(0x44U);
  (void)uc;
  (void)r0;
  (void)r1;
  int size = g_font_size > 0 ? g_font_size : 16;
  return (uint32_t)(size / 4);
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
  /* 层 0（基础层）**不在这里补建**。它由 zm_layer_init_base() 在模拟器启动时
   * 按 RE 的 ZMAEE_IDisplay_New 语义一次建好（载荷 +0x00 = GetBaseLayerDepth()、
   * +0x24 = GetBaseLayerBuffer()）。本函数只做纯粹的"设活动层"。
   * applet 每帧 SetActiveLayer(0)/(1) 交替是正常行为，两层都必须真实存在，
   * 否则它会拿到 -4 退回"全画进层 1"的老路（层不清 → 残影）。 */
  uint32_t ret = zm_layer_SetActiveLayer(uc, display, idx);
  zm_seq_push(1, idx, ret);
  fb_refresh_draw_target(); /* 绘制目标随活动层改变 */
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

/* +0x2C：ZMAEE_IDisplay_UpdateEx(display, rect, count, layerIdList)
 * RE：00029C84。校验顺序与错误码照抄：
 *   display==0 → -4；count>4 → -4；layerList==0 → -4；rect==0 → -4；
 *   *(display+0x0C) != 0 → -1   （该字段是"帧缓冲已锁定"标志）
 * rect = **{x, y, w, h}**（不是 l,t,r,b —— 实测 applet 传 {0,0,240,320}），
 * 并裁剪到 display+0x30/+0x34（= 层 0 的 w/h = 屏幕尺寸）。
 * 合成顺序 = layerList 的顺序；idx > 0xF 与无缓冲层跳过。
 * 本函数以前只接一个参数（当成 fillRect）且只做观测，count 与层列表全丢。 */
uint32_t zm_display_UpdateEx(uc_engine *uc, uint32_t display, uint32_t rect_ptr,
                             uint32_t count, uint32_t list_ptr) {
  zm_display_slot_tick(0x2Cu);
  if (display == 0)
    return (uint32_t)-4;
  if (count > 4u)
    return (uint32_t)-4;
  if (list_ptr == 0)
    return (uint32_t)-4;
  if (rect_ptr == 0)
    return (uint32_t)-4;
  if (uc_read32(uc, display + 0x0C) != 0)
    return (uint32_t)-1;

  int32_t x = (int32_t)uc_read32(uc, rect_ptr);
  int32_t y = (int32_t)uc_read32(uc, rect_ptr + 4);
  int32_t w = (int32_t)uc_read32(uc, rect_ptr + 8);
  int32_t h = (int32_t)uc_read32(uc, rect_ptr + 12);

  uint32_t lst[ZM_LAYER_LIST_MAX] = {0};
  if (count)
    uc_mem_read(uc, list_ptr, lst, (size_t)count * 4u);

  /* 只记录"形态发生变化"的调用 —— 全部 170 次都打会把日志淹掉，
   * 而我们要的是"这个 applet 一共用过几种 (rect,count,list) 组合"。 */
  static uint32_t n = 0;
  static uint32_t sig_prev = 0xFFFFFFFFu;
  static uint32_t sig_cnt = 0;
  n++;
  uint32_t sig = ((uint32_t)x & 0xFFFu) ^ (((uint32_t)y & 0xFFFu) << 12) ^
                 (((uint32_t)w & 0xFFFu) << 8) ^ (((uint32_t)h & 0xFFFu) << 20) ^
                 (count << 4) ^ (lst[0] & 0xF) ^ ((lst[1] & 0xF) << 1) ^
                 ((lst[2] & 0xF) << 2) ^ ((lst[3] & 0xF) << 3);
  if (sig != sig_prev) {
    sig_prev = sig;
    sig_cnt++;
    log_info("[UpdateEx #%u/%u] rect={%d,%d,%d,%d} count=%u 列表={%u,%u,%u,%u}（第 %u 种形态）",
             n, sig_cnt, (int)x, (int)y, (int)w, (int)h, count, lst[0], lst[1],
             lst[2], lst[3], sig_cnt);
    zm_seq_push(3, count, lst[0]);
    zm_seq_dump();
  }
  return update_ex_impl(uc, display, x, y, w, h, lst, count);
}

/* +0x40：SelectFont(this, fontIndex)
 * 真机（00026770）：ctx = *(&dword_64BA8);
 *   if (ctx) { ctx[8] = fontIndex; return 0; } else return -4;
 * 模拟器无按索引字体表（文本绘制走 sp 传 font_size），仅记录选中索引；
 * 上下文恒非空 → 返回 0。 */
uint32_t zm_display_SelectFont(uc_engine *uc, uint32_t display, uint32_t font_idx) {
  zm_display_slot_tick(0x40U);
  (void)uc;
  (void)display;
  g_sel_font = font_idx;
  return 0;
}

/* +0x48：GetFontHeight(this)
 * 真机（000267A8）：sub_26378(this, 0, &h)；成功返回 h（选中字体高度指标），
 * context 空返回 -4。sub_26378 按 dword_64BA8+8 选中字体类型取尺寸(+60/+64/+68)，
 * 再把对应高度指标(+72/+76/+80)写入 *a3。模拟器用当前字体 g_font 的像素行高近似。 */
uint32_t zm_display_GetFontHeight(uc_engine *uc) {
  zm_display_slot_tick(0x48U);
  (void)uc;
  TTF_Font *font = get_font(g_font_size);
  int h = font ? TTF_FontHeight(font) : g_font_size;
  return (uint32_t)h;
}

/* +0x4C：MeasureString(this, str_ptr, len, width_out, sp[metrics_out])
 * 真机（ZMAEE_IDisplay_MeasureString 反编译）：
 *   1) ctx = dword_64BA8；==0 → -4；
 *   2) 按 UCS-2 '\0' 把 len 截到有效字数（len==0 或 *str==0 → 记 0）；
 *   3) 若选中字体类型(ctx[2])==3 → 走自定义字体 vtable（未注册返回 -1）；
 *   4) 否则 ZMAEE_Ucs2_2_Utf8(str, len, buf[512], 0x200)
 *      → NewStringUTF → AndroidAEE_MeasureText(...) → *width_out = 宽（float）；
 *   5) 若 a5 != 0 → sub_26378(this, 0, a5) 写字体高度指标。
 * 模拟器：context 恒非空 → 返回 0；用同一个 ucs2_to_utf8 + 当前 TTF 量宽高。 */
uint32_t zm_display_MeasureString(uc_engine *uc, uint32_t disp, uint32_t str_ptr,
                                  uint32_t len, uint32_t width_out, uint32_t sp) {
  zm_display_slot_tick(0x4CU);
  (void)disp;

  int w = 0;
  if (str_ptr && len) {
    /* 真机顺序：先按 UCS-2 '\0' 定有效长度，再 Ucs2_2_Utf8 转 UTF-8，
     * 然后用它去 NewStringUTF/MeasureText。这里和 DrawText 用**同一个**
     * text_to_utf8（含窄串判定），避免两处编码理解再次分叉。 */
    char utf8[4096];
    size_t ul = text_to_utf8(uc, str_ptr, len, utf8, sizeof(utf8));
    TTF_Font *font = get_font(g_font_size);
    int h = 0;
    if (font)
      TTF_SizeUTF8(font, utf8, &w, &h);
    else
      w = (int)ul * (g_font_size > 0 ? g_font_size : 16);
  }

  if (width_out)
    uc_write32(uc, width_out, (uint32_t)w);

  /* metrics_out = 字体高度（sub_26378 写入 *a3） */
  if (sp) {
    uint32_t metrics_out = uc_read32(uc, sp);
    if (metrics_out) {
      TTF_Font *font = get_font(g_font_size);
      int fh = font ? TTF_FontHeight(font) : (g_font_size > 0 ? g_font_size : 16);
      uc_write32(uc, metrics_out, (uint32_t)fh);
    }
  }
  return 0;
}

/* +0x50：drawText
 *
 * 真机（ZMAEE_IDisplay_DrawText 反编译）签名与语义：
 *   DrawText(this=r0, rect=r1, text=r2 (UCS-2), len=r3,
 *            color=[sp+0], a6=[sp+4], flags=[sp+8])
 *   1) 校验：this/text/rect 为 0 → -4；len==0 或 *text==0 → 直接返回；
 *   2) 按 UCS-2 '\0' 把 len 截到有效字数；
 *   3) **MeasureString(自身, text, len, &w, &h)** 量出文本宽高；
 *   4) 用 flags 把文本在 rect 内定位：
 *        &2 右对齐  &4 水平居中  &0x20 底对齐  &0x10 垂直居中；
 *   5) Ucs2_2_Utf8(text, len, buf, 256) → NewStringUTF → AndroidAEE_GetTextBitmap
 *      （渲染发生在 **Android 侧**，用系统字体，所以真机汉字一定有字形）；
 *   6) ZMAEE_Blt 把这张文字位图贴进当前层。
 *
 * 【2026-09 修正】模拟器以前：把 UCS-2 原始字节当 UTF-8 直接喂 TTF（汉字
 * 两字节被当成非法 UTF-8 → .notdef = 豆腐块），并且把 `flags` 当字号用
 * （实测 applet 传 0x21/0x14/0x11，是"底对齐/居中"而不是 33px/20px）。
 * 现在：先 ucs2_to_utf8 转换 → 字体按 ZM_FONT/CJK 候选挑 → flags 按位定位。
 * a6（[sp+4]）真机透传给 AndroidAEE_GetTextBitmap，含义未定，暂忽略。 */
uint32_t zm_display_DrawText(uc_engine *uc, uint32_t rect_ptr, uint32_t text_ptr,
                             uint32_t text_len, uint32_t sp) {
  zm_display_slot_tick(0x50U);
  if (!rect_ptr || !text_ptr || !text_len)
    return 0;
  char text[512];
  text_to_utf8(uc, text_ptr, text_len, text, sizeof(text));
  if (!text[0])
    return 0;
  uint32_t color = uc_read32(uc, sp);
  uint32_t flags = uc_read32(uc, sp + 8);
  /* 字号来自字体上下文（SelectFont 选中的类型 → 大小），不是栈参数 */
  fb_draw_text(uc, rect_ptr, text, color, g_font_size, flags);
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
  zm_seq_push(2, color, (uint32_t)h);
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
  uint32_t active = uc_read32(uc, DISPLAY + 8);
  if (active > 0xF)
    active = 0;
  uint32_t P = zm_layer_payload(DISPLAY, active);
  uc_write32(uc, P + 0x2C, r1); /* 启用标志：非 0 → 合成走 mask */
  uc_write32(uc, P + 0x30, r2); /* 透明色（ARGB，合成时转 RGB565） */
  log_info("IDisplay.SetTransColor(启用=0x%08X, 色=0x%08X) -> 层%u (+0x2C/+0x30) -> "
           "RGB565 key=0x%04X",
           r1, r2, active, to_rgb565(r2));
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
  fb_refresh_draw_target();
  int w = 0, h = 0;
  const uint8_t *rgba = NULL;
  if (zm_image_get_pixels(r3, &w, &h, &rgba)) {
    /* 首次记录：用来区分"某张图走的是 PNG 路径(DrawImage 0x90) 还是位图路径"，
     * 以及两条路径各自的坐标单位（0x90 传的是 applet 的原始坐标）。 */
    static uint32_t seen[24];
    static int nseen = 0;
    int known = 0;
    for (int i = 0; i < nseen; i++)
      if (seen[i] == r3) {
        known = 1;
        break;
      }
    if (!known && nseen < 24) {
      seen[nseen++] = r3;
      log_info("[DrawImage 首次] obj=0x%X %dx%d 目标=(%d,%d) a5=%d", r3, w, h,
               (int)r1, (int)r2, (int)getArg(uc, 4));
    }
    fb_blit_rgba((int)r1, (int)r2, w, h, rgba);
    return 1;
  }
  log_debug("IDisplay.DrawImage: 对象 0x%X 无像素数据（stub）", r3);
  return 0;
}

/* +0x94 DrawBitmap(this=display, x=r1, y=r2, IBitmap*=r3, rect=[sp+0],
 *                  mask_flag=[sp+4])
 *
 * 真机（ZMAEE_IDisplay_DrawBitmap 反编译）：
 *     if (a4 == 0 || display == 0 || a5 == 0) return;      // rect 必须非空
 *     IBitmap_GetInfo(a4, info);                           // info = 32B 位图头
 *     GDI_BitBlt(层载荷, x, y, info, rect, a6);            // ← a4 换成"信息头"
 * GDI_BitBlt 内部（ZMAEE_GDI_BitBlt 反编译）：
 *     src_fmt = info[2]; tc = info[3];
 *     isMask  = a6 & (tc >= 0 ? 1 : 0);                    // ★ a6 = 抠透明色开关
 *     fn = (isMask ? mask*_func : copy*_func)[src_fmt];
 *     ZMAEE_Blt(bpp[目标色深], bpp[src_fmt], rect, ...);   // ★ 索引里没有 mode
 *
 * 【要点】第 6 参 a6 **不是模式号**，而是 mask/copy 家族选择位；4 种镜像变体
 * （Copy/Mir/Mir90/Mir270 = `byte_5B658[mode+8]` 选组）只挂在 **7 参**的
 * IDisplay::BitBlt 上（那里才有 `a6 <= 7` 的校验）。applet 侧也自洽：
 * 00000506 sub_388 的同一个 wrapper 字段（a1[2]）在 type1 当 DrawBitmap 的
 * a6、在 type2 当 BitBlt 的 a7 —— 两条路径的末参是同一个"mask 开关"。
 *
 * 因此这里恒传 mode=0（无镜像）。真机在 a6=0 时走 copy 家族（只跳 alpha==0，
 * 不抠 tc）；我们统一走"alpha==0 与洋红 key 都跳"的更严版本，肉眼无差，
 * 属有意的简化。00000506 的关卡列表实测走的是 +0x98，0x94 在这几屏没被调用。 */
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

  /* 每个对象第一次被成功解析就记一行 —— 用来把"资源载入地址"和"真的被画了"
   * 对上号（诊断"某张图没显示"时最直接：载入了但从未出现在这里 = 没被画）。 */
  {
    static uint32_t seen[24];
    static uint8_t onscr[24];
    static int nseen = 0;
    int idx = -1;
    for (int i = 0; i < nseen; i++)
      if (seen[i] == obj) {
        idx = i;
        break;
      }
    if (idx < 0 && nseen < 24) {
      idx = nseen;
      seen[nseen++] = obj;
      log_info("[绘制首次] obj=0x%X %dx%d 目标=(%d,%d) mode=%d rect=%s", obj, w, h,
               dx, dy, mode & 7, rect_ptr ? "有" : "无");
    }
    /* 每个对象再补记前 3 次调用，用来判断坐标是"恒定"还是"随时间动画" */
    if (idx >= 0) {
      static uint8_t cnt[24];
      if (cnt[idx] < 3) {
        cnt[idx]++;
        log_info("[绘制#%d] obj=0x%X 目标=(%d,%d)", cnt[idx], obj, dx, dy);
      }
    }
    /* 首次"落在屏内"也算一行：用来区分"从没被画"和"一直被画到屏外" */
    if (idx >= 0 && !onscr[idx] && dx > -w && dx < 240 && dy > -h && dy < 320) {
      onscr[idx] = 1;
      int rl = 0, rt = 0, rr = 0, rb = 0;
      if (rect_ptr) {
        rl = (int)uc_read32(uc, rect_ptr);
        rt = (int)uc_read32(uc, rect_ptr + 4);
        rr = (int)uc_read32(uc, rect_ptr + 8);
        rb = (int)uc_read32(uc, rect_ptr + 12);
      }
      log_info("[绘制入屏] obj=0x%X %dx%d 目标=(%d,%d) rect={%d,%d,%d,%d} mode=%d",
               obj, w, h, dx, dy, rl, rt, rr, rb, mode & 7);
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
  fb_refresh_draw_target();
  /* rect：{left, top, right, bottom}，用于把源图裁剪后画到 (x,y)。
   * 第 6 参（mask 开关）不透传 —— 见上方注释：它不进镜像表。 */
  if (blit_surface_region(uc, r3, (int)r1, (int)r2, getArg(uc, 4), 0))
    return 1;
  log_debug("IDisplay.DrawBitmap: 对象 0x%X 无像素数据（stub）", r3);
  return 0;
}

/* +0x98 DrawBitmapEx(this=display, x=r1, y=r2, bitmap=r3,
 *                    srcRect=[sp+0], mode=[sp+4], mask_flag=[sp+8])
 *
 * 真机（ZMAEE_IDisplay_DrawBitmapEx 反编译）：
 *     if (a4 == 0 || display == 0 || a6 > 7 || a5 == 0) return;   // ★ a6<=7 校验
 *     IBitmap_GetInfo(a4, info);
 *     GDI_BitBlt_Ext(层载荷, x, y, info, rect, a6, a7);           // ★ 与 7 参 BitBlt 同一条路
 * 即 **DrawBitmapEx ≡ DrawBitmap + mode + mask**：
 *     a6 = 模式号（0..7），进 `byte_5B658[a6+8]` 选 Copy/Mir/Mir90/Mir270 变体；
 *     a7 = 上面 DrawBitmap 同款的 mask 开关（不参与几何）。
 * applet 侧自洽（00000506 sub_4A0 type1）：sp+0/4/8 = srcRect / 模式 / [wrapper+8]，
 * 末参正是那个开关。这是主 sprite 绘制入口。
 *
 * 【已证】此处 `getArg(uc,5)` 当 mode 交给 fb_blit_rgba_mode（内部 `mode & 7`
 * 做镜像/转置）与真机一致 —— 之前是本模拟器按调用形状推的假设，现已由上面
 * 的 `a6 <= 7` 校验 + GDI_BitBlt_Ext 落表坐实。 */
uint32_t zm_display_DrawBitmapEx(uc_engine *uc, uint32_t off, uint32_t r0,
                                 uint32_t r1, uint32_t r2, uint32_t r3) {
  (void)off;
  (void)r0;
  fb_refresh_draw_target();
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
    log_warn("LoadBitmap(\"%s\") 头字段异常：w=%d h=%d fmt=%d(0x%X) 文件 %u 字节",
             name, w, h, fmt, fpf, (unsigned)len);
    free(buf);
    return (uint32_t)-1;
  }
  /* 【2026-09 实测修正】文件里的像素块是**紧凑**的，不做 4 字节补齐：
   *   start_register.zbmp 59x13 fmt=1 → 20 + 59*13*2 = 1554 = 文件大小（不是 1556）
   *   game_name.zbmp      231x71 fmt=1 → 20 + 231*71*2 = 32822 = 文件大小
   * 以前按 (px+3)&~3 校验，这两张图被判"数据不足" → 位图槽留空 → applet 后续
   * 拿空槽当对象用 → 崩在 pc=0xA0000010。分配时我们自己的缓冲仍可补齐，
   * 但读取长度必须用紧凑值。 */
  size_t px_raw = (size_t)w * (size_t)h * (size_t)bpp;
  size_t px_alloc = (px_raw + 3u) & ~(size_t)3u;
  if (20u + px_raw > len) {
    log_warn("LoadBitmap(\"%s\") 数据不足：需要 %u，文件只有 %u（%dx%d fmt=%d）",
             name, (unsigned)(20u + px_raw), (unsigned)len, w, h, fmt);
    free(buf);
    return (uint32_t)-1;
  }

  uint32_t gpx = zm_pix_pool_alloc((uint32_t)px_alloc);
  if (!gpx) {
    free(buf);
    return (uint32_t)-1;
  }
  uc_mem_write(uc, gpx, buf + 20, px_raw);

  uint32_t gpal = 0;
  if (palflag && palsize && 20u + px_raw + palsize <= len) {
    gpal = zm_pix_pool_alloc(palsize);
    if (gpal)
      uc_mem_write(uc, gpal, buf + 20 + px_raw, palsize);
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
  fb_refresh_draw_target();
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
   *   → ZMAEE_StretchBlt(&display[52*活动层 + 36], a2, a3, a4, a5)
   * 目标 = **活动层**载荷；a1[9] = 层像素缓冲、a1[5..8] = 该层裁剪区。
   * 实测：本 applet 调 73 次，调用时活动层 = 0（输出层），
   *       a3 = 源位图描述符 {w, h, ZMCF, 透明色, 0,0,0, 像素指针}（与
   *       sub_285D8 里的 srcDesc 同型）。
   * a2 / a4 的字段语义仍未定 —— 本函数是纯观测探针，不做任何绘制。
   *
   * 探针要做三件事：
   *   1) 前 8 次把 a2/a3/a4 各 16 个 dword、目标层全部字段、a5 打全；
   *   2) 统计 a3 的 (宽,高) 分布 —— 若出现 240x320 之类的整层尺寸，
   *      就说明它是"层 1 → 层 0"的整层搬运器；
   *   3) 把所有已加载图像记录的 {宽,高,名字} 打一次，便于把 a3 对上具体资源。
   * 配合日志里已有的 `IImage::Decode -> IBitmap@… pix@0x…` 行，
   * 可以用 a3[7]（像素指针）反查 a3 到底是哪张图。 */
  zm_display_slot_tick(0xB4U);
  uint32_t act = uc_read32(uc, r0 + 8);
  uint32_t P = r0 + 52u * act + 36u;
  uint32_t a5 = getArg(uc, 4);
  static uint32_t n = 0;
  n++;
  if (n <= 8) {
    log_info("StretchBlt #%u display=0x%X 活动层=%u 层载荷=0x%X a2=0x%X a3=0x%X "
             "a4=0x%X a5=0x%X",
             n, r0, act, P, r1, r2, r3, a5);
    log_info("  目标层: fmt=%u x=%d y=%d w=%u h=%u 裁剪=(%u,%u,%u,%u) buf=0x%X "
             "+0x28=%d +0x2A=%d +0x2C=%u +0x30=0x%08X",
             uc_read32(uc, P + 0x00), (int)uc_read32(uc, P + 0x04),
             (int)uc_read32(uc, P + 0x08), uc_read32(uc, P + 0x0C),
             uc_read32(uc, P + 0x10), uc_read32(uc, P + 0x14),
             uc_read32(uc, P + 0x18), uc_read32(uc, P + 0x1C),
             uc_read32(uc, P + 0x20), uc_read32(uc, P + 0x24),
             (int)(int16_t)uc_read32(uc, P + 0x28),
             (int)(int16_t)uc_read32(uc, P + 0x2A), uc_read32(uc, P + 0x2C),
             uc_read32(uc, P + 0x30));
    const char *nm[3] = {"a2", "a3", "a4"};
    uint32_t pa[3] = {r1, r2, r3};
    for (int i = 0; i < 3; i++) {
      uint32_t raw[16] = {0};
      if (uc_mem_read(uc, pa[i], raw, sizeof(raw)) != UC_ERR_OK)
        continue;
      log_info("  %s@0x%X =", nm[i], pa[i]);
      for (int k = 0; k < 16; k += 4)
        log_info("     [%02d..%02d] 0x%08X 0x%08X 0x%08X 0x%08X", k, k + 3,
                 raw[k], raw[k + 1], raw[k + 2], raw[k + 3]);
    }
    /* 已加载图像记录：把 a3 的 (宽,高) 对上去（只打一次） */
    if (n == 1)
      zm_image_dump_pool();
  }
  /* a3 的尺寸分布（换一个尺寸就报一次） */
  {
    uint32_t sw = uc_read32(uc, r2 + 0x00), sh = uc_read32(uc, r2 + 0x04);
    uint32_t sf = uc_read32(uc, r2 + 0x08), sp = uc_read32(uc, r2 + 0x1C);
    static uint32_t last_key = 0xFFFFFFFFu, seen = 0;
    uint32_t key = (sw << 16) | (sh & 0xFFFFu);
    seen++;
    zm_seq_push(4, sw, sh);
    if (key != last_key || seen == 1) {
      last_key = key;
      log_info("[StretchBlt 探针] 第 %u 次：源 %ux%u ZMCF=%u 透明色=0x%08X "
               "像素指针=0x%X（整层搬运? %s）",
               n, sw, sh, sf, uc_read32(uc, r2 + 0x0C), sp,
               (sw == (uint32_t)LAYER_W && sh == (uint32_t)LAYER_H) ? "是 ←"
                                                                    : "否");
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

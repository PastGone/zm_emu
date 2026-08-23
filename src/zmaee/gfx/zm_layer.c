#include "zm_layer.h"

#include "../../emu.h"
#include "../../log/log.h"
#include "../../tool/uc_helper.h"
#include "../core/zm_str.h"
#include "../fs/zm_fs.h"
#include "zm_gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PNG / JPG 解码（系统库 libpng / libjpeg 可用时启用） */
#if defined(HAVE_PNG) || defined(HAVE_JPEG)
#include <setjmp.h>
#endif
#ifdef HAVE_PNG
#include <png.h>
#endif
#ifdef HAVE_JPEG
#include <jpeglib.h>
#endif

/* ==================== 颜色转换 ==================== */

static inline uint16_t argb_to_565(uint32_t argb) {
  uint32_t r = (argb >> 16) & 0xFF;
  uint32_t g = (argb >> 8) & 0xFF;
  uint32_t b = argb & 0xFF;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint32_t rgb565_to_argb(uint16_t p) {
  uint32_t r = (p >> 11) & 0x1F;
  uint32_t g = (p >> 5) & 0x3F;
  uint32_t b = p & 0x1F;
  r = (r << 3) | (r >> 2);
  g = (g << 2) | (g >> 4);
  b = (b << 3) | (b >> 2);
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* ==================== 图层 ==================== */

static ZmLayer g_layers[ZM_MAX_LAYERS];
static uint32_t g_active = 0;
static int g_scr_w = 240, g_scr_h = 320;
int g_layer_head = 0; /* 图层对象头大小，见 zm_layer.h；ZM_LAYER_HEAD 可覆盖 */

void zm_layer_reset(int screen_w, int screen_h) {
  memset(g_layers, 0, sizeof(g_layers));
  g_active = 0;
  if (screen_w > 0)
    g_scr_w = screen_w;
  if (screen_h > 0)
    g_scr_h = screen_h;
  const char *h = getenv("ZM_LAYER_HEAD");
  g_layer_head = h ? (int)strtol(h, NULL, 0) : 0;
}

bool zm_layer_any(void) {
  for (int i = 0; i < ZM_MAX_LAYERS; i++)
    if (g_layers[i].used)
      return true;
  return false;
}

/* 按需创建图层。所有层默认等于屏幕大小（applet 传的 rect 只用来定尺寸）。 */
static ZmLayer *layer_ensure(uint32_t id, int w, int h) {
  if (id >= ZM_MAX_LAYERS) {
    log_warn("[GFX] 图层号 %u 越界（最大 %d），归到 0 层", id,
             ZM_MAX_LAYERS - 1);
    id = 0;
  }
  ZmLayer *L = &g_layers[id];
  if (L->used)
    return L;

  if (w <= 0 || w > 4096)
    w = g_scr_w;
  if (h <= 0 || h > 4096)
    h = g_scr_h;

  uint32_t bytes = (uint32_t)w * (uint32_t)h * 2u + (uint32_t)g_layer_head;
  uint32_t obj = zm_emu_alloc_vram(NULL, bytes);
  if (!obj) {
    log_error("[GFX] 图层 %u 分配 %u 字节失败", id, bytes);
    return NULL;
  }
  /* 清零：头部 + 像素区全清 0（RGB565 的 0 视为透明黑，与 zbmp 约定一致） */
  {
    void *zero = calloc(1, bytes);
    if (zero) {
      uc_mem_write(g_uc, obj, zero, bytes);
      free(zero);
    }
  }
  L->used = true;
  L->w = w;
  L->h = h;
  L->obj = obj;
  L->buf = obj + (uint32_t)g_layer_head;
  L->opaque = false; /* 新层默认透明 overlay，clear 后才变为不透明画布 */
  log_info("[GFX] 创建图层 %u：%dx%d，对象0x%08X 像素0x%08X（RGB565）", id, w, h,
           L->obj, L->buf);
  return L;
}

ZmLayer *zm_layer_get(uint32_t id) {
  if (id >= ZM_MAX_LAYERS)
    return NULL;
  return g_layers[id].used ? &g_layers[id] : NULL;
}

ZmLayer *zm_layer_active(void) {
  ZmLayer *L = zm_layer_get(g_active);
  if (L)
    return L;
  return layer_ensure(g_active, g_scr_w, g_scr_h);
}

/* ---------- 软件光栅化原语（直接写客户机内存） ---------- */

static bool clip_rect(const ZmLayer *L, int *x, int *y, int *w, int *h) {
  if (*w <= 0 || *h <= 0)
    return false;
  if (*x < 0) {
    *w += *x;
    *x = 0;
  }
  if (*y < 0) {
    *h += *y;
    *y = 0;
  }
  if (*x >= L->w || *y >= L->h)
    return false;
  if (*x + *w > L->w)
    *w = L->w - *x;
  if (*y + *h > L->h)
    *h = L->h - *y;
  return *w > 0 && *h > 0;
}

void zm_layer_fill(uc_engine *uc, ZmLayer *L, int x, int y, int w, int h,
                   uint32_t argb) {
  if (!L || !clip_rect(L, &x, &y, &w, &h))
    return;
  uint16_t v = argb_to_565(argb);
  uint16_t *row = malloc((size_t)w * 2);
  if (!row)
    return;
  for (int i = 0; i < w; i++)
    row[i] = v;
  for (int j = 0; j < h; j++) {
    uint32_t off = (uint32_t)(((y + j) * L->w) + x) * 2u;
    uc_mem_write(uc, L->buf + off, row, (size_t)w * 2);
  }
  free(row);
}

void zm_layer_frame(uc_engine *uc, ZmLayer *L, int x, int y, int w, int h,
                    uint32_t argb) {
  if (!L || w <= 0 || h <= 0)
    return;
  zm_layer_fill(uc, L, x, y, w, 1, argb);
  zm_layer_fill(uc, L, x, y + h - 1, w, 1, argb);
  zm_layer_fill(uc, L, x, y, 1, h, argb);
  zm_layer_fill(uc, L, x + w - 1, y, 1, h, argb);
}

/* 把宿主机 ARGB8888 像素按 alpha 混合进图层（用于文本渲染） */
void zm_layer_blit_argb(uc_engine *uc, ZmLayer *L, int dx, int dy,
                        const uint32_t *src, int sw, int sh, int pitch_px,
                        int clip_x, int clip_y, int clip_w, int clip_h) {
  if (!L || !src || sw <= 0 || sh <= 0)
    return;

  /* 目标裁剪区（与图层求交） */
  int cx = clip_x, cy = clip_y, cw = clip_w, ch = clip_h;
  if (cw <= 0 || ch <= 0) {
    cx = 0;
    cy = 0;
    cw = L->w;
    ch = L->h;
  }
  if (!clip_rect(L, &cx, &cy, &cw, &ch))
    return;

  uint16_t *drow = malloc((size_t)sw * 2);
  if (!drow)
    return;

  for (int j = 0; j < sh; j++) {
    int ty = dy + j;
    if (ty < cy || ty >= cy + ch)
      continue;
    /* 该行在目标里的有效 x 区间 */
    int x0 = dx, x1 = dx + sw;
    if (x0 < cx)
      x0 = cx;
    if (x1 > cx + cw)
      x1 = cx + cw;
    if (x1 <= x0)
      continue;
    int n = x1 - x0;
    uint32_t off = (uint32_t)((ty * L->w) + x0) * 2u;
    if (uc_mem_read(uc, L->buf + off, drow, (size_t)n * 2) != UC_ERR_OK)
      continue;
    for (int i = 0; i < n; i++) {
      uint32_t s = src[(size_t)j * pitch_px + (x0 - dx + i)];
      uint32_t a = (s >> 24) & 0xFF;
      if (a == 0)
        continue;
      if (a == 0xFF) {
        drow[i] = argb_to_565(s);
        continue;
      }
      uint32_t d = rgb565_to_argb(drow[i]);
      uint32_t r = (((s >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
      uint32_t g = (((s >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
      uint32_t b = ((s & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
      drow[i] = argb_to_565((r << 16) | (g << 8) | b);
    }
    uc_mem_write(uc, L->buf + off, drow, (size_t)n * 2);
  }
  free(drow);
}

/* 把一块客户机 RGB565 像素贴到图层（0 视为透明） */
static void layer_blit565(uc_engine *uc, ZmLayer *L, int dx, int dy,
                          uint32_t src_buf, int sw, int sh, int sx, int sy,
                          int cw, int ch, const uint8_t *mask) {
  if (!L || !src_buf || sw <= 0 || sh <= 0)
    return;
  if (cw <= 0 || ch <= 0) {
    cw = sw;
    ch = sh;
  }
  if (sx < 0)
    sx = 0;
  if (sy < 0)
    sy = 0;
  if (sx + cw > sw)
    cw = sw - sx;
  if (sy + ch > sh)
    ch = sh - sy;
  if (cw <= 0 || ch <= 0)
    return;

  uint16_t *srow = malloc((size_t)cw * 2);
  uint16_t *drow = malloc((size_t)cw * 2);
  if (!srow || !drow) {
    free(srow);
    free(drow);
    return;
  }

  for (int j = 0; j < ch; j++) {
    int ty = dy + j;
    if (ty < 0 || ty >= L->h)
      continue;
    int x0 = dx, n = cw, soff = 0;
    if (x0 < 0) {
      soff = -x0;
      n -= soff;
      x0 = 0;
    }
    if (x0 + n > L->w)
      n = L->w - x0;
    if (n <= 0)
      continue;

    uint32_t sa = src_buf + (uint32_t)(((sy + j) * sw) + sx + soff) * 2u;
    uint32_t da = L->buf + (uint32_t)((ty * L->w) + x0) * 2u;
    if (uc_mem_read(uc, sa, srow, (size_t)n * 2) != UC_ERR_OK)
      continue;
    if (uc_mem_read(uc, da, drow, (size_t)n * 2) != UC_ERR_OK)
      continue;
    for (int i = 0; i < n; i++) {
      if (mask) {
        size_t mi = (size_t)((sy + j) * sw + sx + soff + i);
        if (!mask[mi])
          continue;
      } else if (srow[i] == 0) {
        continue; /* 0 视为透明 */
      }
      drow[i] = srow[i];
    }
    uc_mem_write(uc, da, drow, (size_t)n * 2);
  }
  free(srow);
  free(drow);
}

/* ---------- 合成 ---------- */

void zm_layer_composite(uc_engine *uc, uint32_t *out, int w, int h) {
  if (!out || w <= 0 || h <= 0)
    return;
  for (int i = 0; i < w * h; i++)
    out[i] = 0xFF000000u;

  uint16_t *row = malloc((size_t)w * 2);
  if (!row)
    return;

  for (int li = 0; li < ZM_MAX_LAYERS; li++) {
    ZmLayer *L = &g_layers[li];
    if (!L->used)
      continue;
    int cw = L->w < w ? L->w : w;
    int chh = L->h < h ? L->h : h;
    for (int y = 0; y < chh; y++) {
      if (uc_mem_read(uc, L->buf + (uint32_t)(y * L->w) * 2u, row,
                      (size_t)cw * 2) != UC_ERR_OK)
        continue;
      uint32_t *dst = out + (size_t)y * w;
      for (int x = 0; x < cw; x++) {
        /* 第 0 层是底板，整幅覆盖；其余层仅"非不透明层"的 0 像素视为透明
         * （不透明层由 clearLayer 置位，其黑色是真实颜色——00000405 的红底黑字）。 */
        if (li != 0 && row[x] == 0 && !L->opaque)
          continue;
        dst[x] = rgb565_to_argb(row[x]);
      }
    }
  }
  free(row);
}

void zm_layer_debug_dump(uc_engine *uc) {
  for (int li = 0; li < ZM_MAX_LAYERS; li++) {
    ZmLayer *L = &g_layers[li];
    if (!L->used)
      continue;
    uint32_t nz = 0;
    int w = L->w < 4096 ? L->w : 4096;
    int h = L->h < 4096 ? L->h : 4096;
    uint16_t *row = malloc((size_t)w * 2);
    if (row) {
      for (int y = 0; y < h; y++) {
        if (uc_mem_read(uc, L->buf + (uint32_t)(y * L->w) * 2u, row,
                        (size_t)w * 2) != UC_ERR_OK)
          break;
        for (int x = 0; x < w; x++)
          if (row[x])
            nz++;
      }
      free(row);
    }
    uint8_t head[16];
    if (uc_mem_read(uc, L->buf, head, sizeof(head)) == UC_ERR_OK) {
      char hx[48];
      for (int i = 0; i < 16; i++)
        sprintf(hx + i * 3, "%02X ", head[i]);
      log_warn("[layer-dump] L%u buf=0x%08X %dx%d opaque=%d nonzero=%u head=%s",
               li, L->buf, L->w, L->h, L->opaque, nz, hx);
    }
  }
}

/* ==================== 图层 API（GFX_VT） ==================== */

/* rect_ptr 指向 {x, y, w, h} */
static void read_rect(uc_engine *uc, uint32_t p, int *x, int *y, int *w,
                      int *h) {
  *x = *y = 0;
  *w = *h = 0;
  if (!p)
    return;
  *x = (int)uc_read32(uc, p);
  *y = (int)uc_read32(uc, p + 4);
  *w = (int)uc_read32(uc, p + 8);
  *h = (int)uc_read32(uc, p + 12);
}

uint32_t zm_gfx_create_layer(uc_engine *uc, uint32_t id, uint32_t rect_ptr) {
  int x, y, w, h;
  read_rect(uc, rect_ptr, &x, &y, &w, &h);
  ZmLayer *L = layer_ensure(id, w, h);
  return L ? 0 : 1; /* 0 = 成功，与 applet 的 "ret==0" 判定一致 */
}

uint32_t zm_gfx_free_layers(uc_engine *uc) {
  (void)uc;
  /* 只把非 0 层标记为释放：0 层是屏幕底板，释放掉会让后续绘制无处落地。
   * 客户机堆是线性分配器，这里不实际回收内存。 */
  for (int i = 1; i < ZM_MAX_LAYERS; i++)
    g_layers[i].used = false;
  g_active = 0;
  log_debug("[GFX] freeAllLayer");
  return 0;
}

uint32_t zm_gfx_active_layer(uc_engine *uc, uint32_t id) {
  if (id >= ZM_MAX_LAYERS) {
    log_warn("[GFX] setActiveLayer(%u) 越界，忽略", id);
    return 0;
  }
  layer_ensure(id, g_scr_w, g_scr_h);
  g_active = id;
  log_debug("[GFX] setActiveLayer(%u)", id);
  return 0;
}

uint32_t zm_gfx_layer_info(uc_engine *uc, uint32_t id, uint32_t info_ptr) {
  ZmLayer *L = layer_ensure(id, g_scr_w, g_scr_h);
  if (!L || !info_ptr)
    return 1;
  uint8_t zero[ZM_LAYERINFO_SIZE] = {0};
  uc_mem_write(uc, info_ptr, zero, sizeof(zero));
  uc_write32(uc, info_ptr + ZM_LAYERINFO_W, (uint32_t)L->w);
  uc_write32(uc, info_ptr + ZM_LAYERINFO_H, (uint32_t)L->h);
  uc_write32(uc, info_ptr + ZM_LAYERINFO_BUF, L->obj);
  log_warn("[GFX-DIAG] getLayerInfo(%u) -> 0x%08X", id, L->buf);
  return 0;
}

uint32_t zm_gfx_update_layer(uc_engine *uc, uint32_t id, uint32_t x, uint32_t y,
                             uint32_t sp) {
  (void)x;
  (void)y;
  (void)sp;
  layer_ensure(id, g_scr_w, g_scr_h);
  /* applet 主动要求刷新：把当前所有层合成后送显 */
  zm_gfx_present();
  return 0;
}

uint32_t zm_gfx_get_active_layer(uc_engine *uc) {
  (void)uc;
  return g_active;
}

uint32_t zm_gfx_clear_layer(uc_engine *uc, uint32_t id, uint32_t color) {
  ZmLayer *L = layer_ensure(id, g_scr_w, g_scr_h);
  if (!L)
    return 1;
  /* clear 是整层填充：该层变为不透明画布，黑色(0)是真实颜色而非透明
   * （00000405 在红 clear 后的层上画黑字，若 0 仍视为透明则文字消失）。 */
  L->opaque = true;
  zm_layer_fill(uc, L, 0, 0, L->w, L->h, color);
  return 0;
}

/* ==================== 图片 ==================== */

/* ---- zbmp 解码（格式见 go_tools/zbmp_decoder.go） ---- */
typedef struct {
  int w, h;
  uint16_t *px;  /* RGB565 */
  uint8_t *mask; /* 1=不透明 */
} HostImage;

static void host_image_free(HostImage *im) {
  free(im->px);
  free(im->mask);
  im->px = NULL;
  im->mask = NULL;
}

static bool decode_zbmp(const uint8_t *data, size_t len, HostImage *out) {
  if (len < 16 || memcmp(data, "ZMBM", 4) != 0)
    return false;
  int w = data[4] | (data[5] << 8);
  int h = data[6] | (data[7] << 8);
  uint32_t flags = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                   ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return false;

  const uint8_t *p = data + 16;
  size_t avail = len - 16;
  size_t npx = (size_t)w * (size_t)h;

  int bpp;
  if ((flags & 0x0100) || (flags & 0x0002) || avail >= npx * 4)
    bpp = 4;
  else
    bpp = 2;

  out->w = w;
  out->h = h;
  out->px = calloc(npx, 2);
  out->mask = calloc(npx, 1);
  if (!out->px || !out->mask) {
    host_image_free(out);
    return false;
  }

  size_t have = avail / (size_t)bpp;
  if (have > npx)
    have = npx;
  for (size_t i = 0; i < have; i++) {
    if (bpp == 4) {
      uint8_t b = p[i * 4 + 0], g = p[i * 4 + 1], r = p[i * 4 + 2],
              a = p[i * 4 + 3];
      out->px[i] = argb_to_565(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
      out->mask[i] = a ? 1 : 0;
    } else {
      uint16_t v = (uint16_t)(p[i * 2] | (p[i * 2 + 1] << 8));
      out->px[i] = v;
      out->mask[i] = v ? 1 : 0;
    }
  }
  log_info("[GFX] zbmp 解码成功：%dx%d bpp=%d flags=0x%X", w, h, bpp, flags);
  return true;
}

/* ---- .zmspx 精灵解码（魔数 "zms2"）----
 * 头结构（小端，经实测样本反推）：
 *   0x00  "zms2"
 *   0x04  version = 1
 *   0x0C  帧数 / 帧表项数
 *   0x1C  width  (uint16)
 *   0x1E  height (uint16)
 *   0x20  format (3 = RGBA)
 *   0x24  pixel-data 偏移（相对文件头）
 * 像素区从 data_off 起，按 RGBA (4B/px) 排列；整张作为一张精灵表，
 * 子帧由调用方的帧表偏移切分。这里整张解出即可。 */
static bool decode_zmspx(const uint8_t *data, size_t len, HostImage *out) {
  if (len < 0x30)
    return false;
  int w = data[0x1C] | (data[0x1D] << 8);
  int h = data[0x1E] | (data[0x1F] << 8);
  uint32_t data_off = (uint32_t)data[0x24] | ((uint32_t)data[0x25] << 8) |
                      ((uint32_t)data[0x26] << 16) | ((uint32_t)data[0x27] << 24);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return false;
  if (data_off < 0x28 || data_off + (size_t)w * h * 4 > len)
    return false;

  const uint8_t *p = data + data_off;
  size_t npx = (size_t)w * (size_t)h;
  out->w = w;
  out->h = h;
  out->px = calloc(npx, 2);
  out->mask = calloc(npx, 1);
  if (!out->px || !out->mask) {
    host_image_free(out);
    return false;
  }
  for (size_t i = 0; i < npx; i++) {
    uint8_t r = p[i * 4 + 0], g = p[i * 4 + 1], b = p[i * 4 + 2],
            a = p[i * 4 + 3];
    out->px[i] = argb_to_565(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    out->mask[i] = a ? 1 : 0;
  }
  log_info("[GFX] zmspx 解码成功：%dx%d off=0x%X", w, h, data_off);
  return true;
}

/* ---- PNG 解码（libpng）----
 * 统一展开成 8bit RGBA，再转 RGB565 + alpha 掩码。 */
#ifdef HAVE_PNG
struct png_mem_reader {
  const uint8_t *p;
  size_t n;
  size_t off;
};

static void png_read_cb(png_structp png, png_bytep out, png_size_t len) {
  struct png_mem_reader *r = (struct png_mem_reader *)png_get_io_ptr(png);
  size_t avail = r->n - r->off;
  size_t n = (avail < len) ? avail : len;
  memcpy(out, r->p + r->off, n);
  r->off += n;
  if (n < len)
    memset(out + n, 0, len - n);
}

static bool decode_png(const uint8_t *data, size_t len, HostImage *out) {
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL,
                                           NULL);
  if (!png)
    return false;
  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, NULL, NULL);
    return false;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, NULL);
    return false;
  }

  struct png_mem_reader r = {data, len, 0};
  png_set_read_fn(png, &r, png_read_cb);
  png_read_info(png, info);

  int w = (int)png_get_image_width(png, info);
  int h = (int)png_get_image_height(png, info);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
    png_destroy_read_struct(&png, &info, NULL);
    return false;
  }

  png_byte ct = png_get_color_type(png, info);
  if (ct == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  if (!(ct & PNG_COLOR_MASK_ALPHA))
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(png, info);

  size_t rowbytes = png_get_rowbytes(png, info);
  uint8_t *rows = malloc(rowbytes * (size_t)h);
  png_bytep *rowp = malloc(sizeof(png_bytep) * (size_t)h);
  if (!rows || !rowp) {
    free(rows);
    free(rowp);
    png_destroy_read_struct(&png, &info, NULL);
    return false;
  }
  for (int y = 0; y < h; y++)
    rowp[y] = rows + (size_t)y * rowbytes;
  png_read_image(png, rowp);
  png_read_end(png, NULL);

  size_t npx = (size_t)w * (size_t)h;
  out->w = w;
  out->h = h;
  out->px = calloc(npx, 2);
  out->mask = calloc(npx, 1);
  if (out->px && out->mask) {
    for (int y = 0; y < h; y++) {
      const uint8_t *row = rows + (size_t)y * rowbytes;
      for (int x = 0; x < w; x++) {
        const uint8_t *px = row + (size_t)x * 4;
        size_t i = (size_t)y * w + x;
        out->px[i] =
            argb_to_565(((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2]);
        out->mask[i] = px[3] ? 1 : 0;
      }
    }
  } else {
    host_image_free(out);
  }
  free(rows);
  free(rowp);
  png_destroy_read_struct(&png, &info, NULL);
  if (!out->px || !out->mask)
    return false;
  log_info("[GFX] PNG 解码成功：%dx%d", w, h);
  return true;
}
#endif /* HAVE_PNG */

/* ---- JPEG 解码（libjpeg）---- */
#ifdef HAVE_JPEG
static bool decode_jpg(const uint8_t *data, size_t len, HostImage *out) {
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, (unsigned char *)data, len);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);
  int w = (int)cinfo.output_width;
  int h = (int)cinfo.output_height;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  int rowstride = w * 3;
  uint8_t *buf = malloc((size_t)rowstride * (size_t)h);
  JSAMPROW row = malloc((size_t)rowstride);
  if (!buf || !row) {
    free(buf);
    free(row);
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  while (cinfo.output_scanline < (JDIMENSION)h) {
    jpeg_read_scanlines(&cinfo, &row, 1);
    memcpy(buf + (size_t)(cinfo.output_scanline - 1) * rowstride, row,
           (size_t)rowstride);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

  size_t npx = (size_t)w * (size_t)h;
  out->w = w;
  out->h = h;
  out->px = calloc(npx, 2);
  out->mask = calloc(npx, 1);
  if (out->px && out->mask) {
    for (size_t i = 0; i < npx; i++) {
      out->px[i] = argb_to_565(((uint32_t)buf[i * 3] << 16) |
                               ((uint32_t)buf[i * 3 + 1] << 8) | buf[i * 3 + 2]);
      out->mask[i] = 1; /* JPG 不透明 */
    }
  } else {
    host_image_free(out);
  }
  free(buf);
  free(row);
  if (!out->px || !out->mask)
    return false;
  log_info("[GFX] JPEG 解码成功：%dx%d", w, h);
  return true;
}
#endif /* HAVE_JPEG */

/* ---- GIF 解码（GIF89a/87a，单帧，支持全局/局部调色板与透明色）---- */
static bool gif_lzw_decode(const uint8_t *data, size_t len, int min_code,
                           int npix, uint8_t *out) {
  int clear = 1 << min_code;
  int eoi = clear + 1;
  int codesize = min_code + 1;
  int next_code = eoi + 1;
  int dprefix[4096];
  uint8_t dsuffix[4096];
  for (int i = 0; i < clear; i++) {
    dprefix[i] = -1;
    dsuffix[i] = (uint8_t)i;
  }
  int bitpos = 0;
  int totalbits = (int)(len * 8);
  int prev = -1;
  int outpos = 0;
  int stack[4096];
  while (outpos < npix) {
    if (bitpos + codesize > totalbits)
      break;
    int code = 0;
    for (int i = 0; i < codesize; i++) {
      int byte = data[bitpos >> 3];
      int bit = (byte >> (bitpos & 7)) & 1;
      code |= bit << i;
      bitpos++;
    }
    if (code == clear) {
      next_code = eoi + 1;
      codesize = min_code + 1;
      prev = -1;
      continue;
    }
    if (code == eoi)
      break;
    int cur;
    uint8_t fb;
    bool extra = false;
    if (prev == -1)
      cur = code;
    else if (code < next_code)
      cur = code;
    else {
      cur = prev;
      extra = true;
    }
    int sp = 0;
    int k = cur;
    while (k >= 0 && sp < 4096) {
      stack[sp++] = dsuffix[k];
      k = dprefix[k];
    }
    fb = stack[sp - 1];
    for (int i = sp - 1; i >= 0 && outpos < npix; i--)
      out[outpos++] = (uint8_t)stack[i];
    if (extra && outpos < npix)
      out[outpos++] = fb;
    if (prev >= 0 && next_code < 4096) {
      dprefix[next_code] = prev;
      dsuffix[next_code] = fb;
      next_code++;
      if (next_code == (1 << codesize) && codesize < 12)
        codesize++;
    }
    prev = cur;
  }
  return outpos >= npix;
}

static bool decode_gif(const uint8_t *data, size_t len, HostImage *out) {
  if (len < 13 || memcmp(data, "GIF", 3) != 0)
    return false;
  int w = (int)data[6] | ((int)data[7] << 8);
  int h = (int)data[8] | ((int)data[9] << 8);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return false;
  int packed = data[10];
  int pos = 13;
  uint8_t gpal[256 * 3];
  int gpal_n = 0;
  if (packed & 0x80) {
    gpal_n = 1 << ((packed & 7) + 1);
    memcpy(gpal, data + 13, (size_t)gpal_n * 3);
    pos = 13 + gpal_n * 3;
  }
  int transparent = -1;
  uint8_t lpal[256 * 3];
  while (pos < (int)len) {
    int b = data[pos++];
    if (b == 0x3B)
      break;
    if (b == 0x21) {
      int label = data[pos++];
      if (label == 0xF9) {
        int sz = data[pos++];
        int flags = data[pos];
        if (flags & 0x01)
          transparent = data[pos + 3];
        pos += sz;
        if (pos < (int)len && data[pos] == 0)
          pos++;
      } else {
        while (pos < (int)len) {
          int sz = data[pos++];
          if (sz == 0)
            break;
          pos += sz;
        }
      }
      continue;
    }
    if (b == 0x2C) {
      int iw = (int)data[pos + 4] | ((int)data[pos + 5] << 8);
      int ih = (int)data[pos + 6] | ((int)data[pos + 7] << 8);
      int ipacked = data[pos + 8];
      pos += 9;
      uint8_t *pal = gpal;
      int pal_n = gpal_n;
      if (ipacked & 0x80) {
        pal_n = 1 << ((ipacked & 7) + 1);
        memcpy(lpal, data + pos, (size_t)pal_n * 3);
        pos += pal_n * 3;
        pal = lpal;
      }
      int min_code = data[pos++];
      size_t cap = 1 << 16, lzwlen = 0;
      uint8_t *lzw = malloc(cap);
      while (pos < (int)len) {
        int sz = data[pos++];
        if (sz == 0)
          break;
        if (lzwlen + (size_t)sz > cap) {
          cap = lzwlen + (size_t)sz + (1 << 16);
          uint8_t *nw = realloc(lzw, cap);
          if (!nw) {
            free(lzw);
            return false;
          }
          lzw = nw;
        }
        memcpy(lzw + lzwlen, data + pos, (size_t)sz);
        lzwlen += (size_t)sz;
        pos += sz;
      }
      bool interlaced = (ipacked & 0x40) != 0;
      int *rows = malloc((size_t)ih * sizeof(int));
      int n = 0;
      if (interlaced) {
        int starts[] = {0, 4, 2, 1}, steps[] = {8, 8, 4, 2};
        for (int p = 0; p < 4; p++)
          for (int y = starts[p]; y < ih; y += steps[p])
            rows[n++] = y;
      } else {
        for (int y = 0; y < ih; y++)
          rows[n++] = y;
      }
      uint8_t *raw = malloc((size_t)iw * ih);
      uint8_t *idx = malloc((size_t)iw * ih);
      bool ok = raw && idx && gif_lzw_decode(lzw, lzwlen, min_code, iw * ih, raw);
      if (ok) {
        for (int r = 0; r < ih; r++)
          memcpy(idx + (size_t)rows[r] * iw, raw + (size_t)r * iw, (size_t)iw);
      }
      free(lzw);
      free(raw);
      free(rows);
      if (!ok) {
        free(idx);
        return false;
      }
      size_t npx = (size_t)iw * ih;
      out->w = iw;
      out->h = ih;
      out->px = calloc(npx, 2);
      out->mask = calloc(npx, 1);
      if (!out->px || !out->mask) {
        host_image_free(out);
        free(idx);
        return false;
      }
      for (size_t i = 0; i < npx; i++) {
        int c = idx[i];
        if (c < 0 || c >= pal_n) {
          out->px[i] = 0;
          out->mask[i] = 0;
          continue;
        }
        uint8_t r = pal[c * 3], g = pal[c * 3 + 1], b = pal[c * 3 + 2];
        out->px[i] =
            (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        out->mask[i] = (c == transparent) ? 0 : 1;
      }
      free(idx);
      return true;
    }
    break;
  }
  return false;
}

/* 读取整个文件到内存（调用方 free） */
static uint8_t *slurp(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 32 * 1024 * 1024) {
    fclose(f);
    return NULL;
  }
  uint8_t *buf = malloc((size_t)n);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t rd = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *out_len = rd;
  return buf;
}

/* 在客户机堆上造一个 IImage 对象 */
static uint32_t image_alloc_obj(uc_engine *uc) {
  uint32_t obj = zm_emu_alloc_guest(NULL, ZM_IMG_SIZE);
  if (!obj)
    return 0;
  uint8_t zero[ZM_IMG_SIZE] = {0};
  uc_mem_write(uc, obj, zero, sizeof(zero));
  uc_write32(uc, obj + ZM_IMG_VPTR, IMAGE_VT);
  uc_write32(uc, obj + ZM_IMG_MAGIC, ZM_IMG_MAGIC_VAL);
  return obj;
}

static bool image_is_ours(uc_engine *uc, uint32_t obj) {
  if (!obj)
    return false;
  uint32_t vptr = 0, magic = 0;
  if (uc_mem_read(uc, obj + ZM_IMG_VPTR, &vptr, 4) != UC_ERR_OK)
    return false;
  if (uc_mem_read(uc, obj + ZM_IMG_MAGIC, &magic, 4) != UC_ERR_OK)
    return false;
  return vptr == IMAGE_VT && magic == ZM_IMG_MAGIC_VAL;
}

/* 把宿主机解码结果搬进客户机内存并写入 IImage 对象 */
static bool image_store(uc_engine *uc, uint32_t obj, HostImage *im) {
  size_t npx = (size_t)im->w * (size_t)im->h;
  /* 像素与掩码放显存，避免和 applet 抢客户机堆 */
  uint32_t gbuf = zm_emu_alloc_vram(im->px, (uint32_t)(npx * 2));
  uint32_t gmask = zm_emu_alloc_vram(im->mask, (uint32_t)npx);
  if (!gbuf || !gmask)
    return false;
  uc_write32(uc, obj + ZM_IMG_W, (uint32_t)im->w);
  uc_write32(uc, obj + ZM_IMG_H, (uint32_t)im->h);
  uc_write32(uc, obj + ZM_IMG_BUF, gbuf);
  uc_write32(uc, obj + ZM_IMG_MASK, gmask);
  return true;
}

/* 解析路径 → 解码 → 填充 obj。成功返回 true。 */
static bool image_load_path(uc_engine *uc, uint32_t obj, const char *name) {
  char disk[2048];
  if (!name || !name[0])
    return false;
  if (!zm_fs_resolve_read(name, disk, sizeof(disk))) {
    log_info("[GFX] 图片 \"%s\" 在素材目录中不存在，按加载失败处理", name);
    return false;
  }
  size_t len = 0;
  uint8_t *data = slurp(disk, &len);
  if (!data)
    return false;

  HostImage im = {0, 0, NULL, NULL};
  bool ok = false;
  if (len >= 6 && (memcmp(data, "GIF89a", 6) == 0 ||
                   memcmp(data, "GIF87a", 6) == 0))
    ok = decode_gif(data, len, &im);
  else if (len >= 4 && memcmp(data, "ZMBM", 4) == 0)
    ok = decode_zbmp(data, len, &im);
  else if (len >= 4 && memcmp(data, "zms2", 4) == 0)
    ok = decode_zmspx(data, len, &im);
  else if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
           data[3] == 'G')
#ifdef HAVE_PNG
    ok = decode_png(data, len, &im);
#else
    ok = false;
#endif
  else if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
#ifdef HAVE_JPEG
    ok = decode_jpg(data, len, &im);
#else
    ok = false;
#endif
  free(data);
  if (!ok) {
    log_info("[GFX] \"%s\" 不是可识别的图片格式（支持 zbmp/zmspx/png/jpg），按加载失败处理",
             disk);
    return false;
  }
  ok = image_store(uc, obj, &im);
  host_image_free(&im);
  return ok;
}

uint32_t zm_gfx_image_new(uc_engine *uc, uint32_t out_ptr) {
  uint32_t obj = image_alloc_obj(uc);
  if (!obj)
    return 1;
  if (out_ptr)
    uc_write32(uc, out_ptr, obj);
  log_debug("[GFX] createImage -> 0x%08X", obj);
  return 0;
}

uint32_t zm_gfx_image_file(uc_engine *uc, uint32_t path_ptr,
                           uint32_t out_ptr) {
  char name[512];
  zm_read_str_obj(uc, path_ptr, name, sizeof(name));

  uint32_t obj = image_alloc_obj(uc);
  if (!obj)
    return 1;
  if (!image_load_path(uc, obj, name)) {
    if (out_ptr)
      uc_write32(uc, out_ptr, 0);
    return 1; /* 非 0 = 失败，applet 会跳过后续绘制 */
  }
  if (out_ptr)
    uc_write32(uc, out_ptr, obj);
  log_info("[GFX] createImageFromFile(\"%s\") -> 0x%08X", name, obj);
  return 0;
}

uint32_t zm_img_release(uc_engine *uc, uint32_t img) {
  (void)uc;
  (void)img;
  return 0; /* 线性分配器，不回收 */
}

uint32_t zm_img_load_file(uc_engine *uc, uint32_t img, uint32_t path_ptr,
                          uint32_t path_len) {
  (void)path_len;
  bool ours = image_is_ours(uc, img);
  uint8_t raw[32];
  char hex[80] = {0};
  uint32_t dp = 0;
  uint8_t iraw[0x20];
  char ihex[80] = {0};
  if (uc_mem_read(uc, img, iraw, sizeof(iraw)) == UC_ERR_OK)
    for (int i = 0; i < (int)sizeof(iraw); i++)
      sprintf(ihex + i * 2, "%02X", iraw[i]);
  if (uc_mem_read(uc, path_ptr, raw, sizeof(raw)) == UC_ERR_OK) {
    for (int i = 0; i < (int)sizeof(raw); i++)
      sprintf(hex + i * 2, "%02X", raw[i]);
    uc_mem_read(uc, path_ptr, &dp, 4);
  }
  log_info("[DIAG-load] img=0x%X ours=%d img_raw=%s", img, ours, ihex);
  log_info("[DIAG-load] path_ptr=0x%X raw=%s data_ptr=0x%X", path_ptr, hex, dp);
  if (dp) {
    uint8_t raw2[64];
    char hex2[160] = {0};
    if (uc_mem_read(uc, dp, raw2, sizeof(raw2)) == UC_ERR_OK) {
      for (int i = 0; i < (int)sizeof(raw2); i++)
        sprintf(hex2 + i * 2, "%02X", raw2[i]);
      char asc[68];
      for (int i = 0; i < 64; i++)
        asc[i] = (raw2[i] >= 0x20 && raw2[i] < 0x80) ? (char)raw2[i] : '.';
      asc[64] = '\0';
      log_info("[DIAG-load] data_ptr@0x%X hex=%s", dp, hex2);
      log_info("[DIAG-load] data_ptr@0x%X ascii=\"%s\"", dp, asc);
      /* 前 8 个 dword，判断是否为 vtable（SHIM 区函数指针） */
      for (int i = 0; i < 8; i++) {
        uint32_t d;
        uc_mem_read(uc, dp + i * 4, &d, 4);
        log_info("[DIAG-load]   dword[%d]=0x%X %s", i, d,
                 (d >= 0x480000 && d < 0x106A0000) ? "(SHIM-vtable?)" : "");
      }
    }
  }
  if (!ours)
    return 1;
  char name[512];
  zm_read_str_obj(uc, path_ptr, name, sizeof(name));
  log_info("[DIAG-load] resolved name=\"%s\"", name);
  if (!image_load_path(uc, img, name))
    return 1;
  log_info("[GFX] image.load(\"%s\") ok", name);
  return 0;
}

uint32_t zm_img_get_size(uc_engine *uc, uint32_t img, uint32_t out_ptr) {
  uint32_t w = 0, h = 0;
  if (image_is_ours(uc, img)) {
    w = uc_read32(uc, img + ZM_IMG_W);
    h = uc_read32(uc, img + ZM_IMG_H);
  }
  /* 关键：宽高必须 >0。00000405 用 "x += img_w" 平铺背景，
   * 返回 0 会让它陷入永不前进的死循环。 */
  if (w == 0)
    w = 1;
  if (h == 0)
    h = 1;
  if (out_ptr) {
    uc_write32(uc, out_ptr, w);
    uc_write32(uc, out_ptr + 4, h);
  }
  return 0;
}

uint32_t zm_img_make_desc(uc_engine *uc, uint32_t img, uint32_t out_ptr) {
  if (!image_is_ours(uc, img))
    return 1;
  uint32_t w = uc_read32(uc, img + ZM_IMG_W);
  uint32_t h = uc_read32(uc, img + ZM_IMG_H);
  uint32_t buf = uc_read32(uc, img + ZM_IMG_BUF);
  if (!w || !h || !buf)
    return 1;

  uint32_t desc = zm_emu_alloc_guest(NULL, ZM_IMGDESC_SIZE);
  if (!desc)
    return 1;
  uint8_t zero[ZM_IMGDESC_SIZE] = {0};
  uc_mem_write(uc, desc, zero, sizeof(zero));
  uc_write32(uc, desc + ZM_IMGDESC_W, w);
  uc_write32(uc, desc + ZM_IMGDESC_H, h);
  uc_write32(uc, desc + ZM_IMGDESC_FMT, 1);
  uc_write32(uc, desc + ZM_IMGDESC_BUF, buf);
  if (out_ptr)
    uc_write32(uc, out_ptr, desc);
  return 0;
}

/* drawImage：obj 既可能是我们造的 IImage，也可能是 applet 自己拼的
 * 0x20 字节描述符（00000506 就是把图层缓冲包成描述符再画）。 */
uint32_t zm_gfx_draw_image(uc_engine *uc, uint32_t x, uint32_t y,
                           uint32_t obj_ptr, uint32_t rect_ptr) {
  ZmLayer *L = zm_layer_active();
  if (!L || !obj_ptr)
    return 0;

  int sw = 0, sh = 0;
  uint32_t buf = 0, maskp = 0;

  if (image_is_ours(uc, obj_ptr)) {
    sw = (int)uc_read32(uc, obj_ptr + ZM_IMG_W);
    sh = (int)uc_read32(uc, obj_ptr + ZM_IMG_H);
    buf = uc_read32(uc, obj_ptr + ZM_IMG_BUF);
    maskp = uc_read32(uc, obj_ptr + ZM_IMG_MASK);
  } else {
    sw = (int)uc_read32(uc, obj_ptr + ZM_IMGDESC_W);
    sh = (int)uc_read32(uc, obj_ptr + ZM_IMGDESC_H);
    buf = uc_read32(uc, obj_ptr + ZM_IMGDESC_BUF);
  }
  if (sw <= 0 || sh <= 0 || sw > 4096 || sh > 4096 || !buf)
    return 0;

  int sx = 0, sy = 0, cw = sw, ch = sh;
  if (rect_ptr) {
    int rx, ry, rw, rh;
    read_rect(uc, rect_ptr, &rx, &ry, &rw, &rh);
    if (rw > 0 && rh > 0) {
      sx = rx;
      sy = ry;
      cw = rw;
      ch = rh;
    }
  }

  uint8_t *mask = NULL;
  if (maskp) {
    mask = malloc((size_t)sw * (size_t)sh);
    if (mask && uc_mem_read(uc, maskp, mask, (size_t)sw * (size_t)sh) !=
                    UC_ERR_OK) {
      free(mask);
      mask = NULL;
    }
  }
  layer_blit565(uc, L, (int)x, (int)y, buf, sw, sh, sx, sy, cw, ch, mask);
  free(mask);
  return 0;
}

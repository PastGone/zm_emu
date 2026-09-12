#include "zm_image.h"

#include "../../emu.h"
#include "../../log/log.h"
#include "../../tool/uc_helper.h"
#include "../core/zm_str.h"
#include "../fs/zm_file_mgr.h"
#include "zm_display.h" /* 软件帧缓冲：贴 GDI_Surface 用 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>

/* =========================================================================
 * IImage / IBitmap 对象池
 * ========================================================================= */

typedef enum { REC_ENTRY = 0, REC_SURF = 1, REC_BITMAP = 2 } rec_kind;

typedef struct {
  int used;
  rec_kind kind;   /* ENTRY=CreateImage 造的数据对象；SURF=entry+8 绘制对象；
                      BITMAP=Decode 出的位图 */
  int refcnt;
  int w, h;
  uint8_t *rgba;   /* w*h*4，RGBA8888（ENTRY 与其 SURF 共享，仅 SURF 释放） */
  char name[160];  /* 最近一次 SetData 的文件名（调试用） */
} zm_img_rec;

static zm_img_rec g_img[IMAGE_SLOT_COUNT];   /* 对应 IMAGE_POOL  */
static zm_img_rec g_bmp[BITMAP_SLOT_COUNT];  /* 对应 BITMAP_POOL */
static zm_img_rec g_bmp_single;              /* 旧 BITMAP 单例 */

static uint32_t img_addr(int i) {
  return IMAGE_POOL + (uint32_t)i * IMAGE_SLOT_SIZE;
}
static uint32_t bmp_addr(int i) {
  return BITMAP_POOL + (uint32_t)i * BITMAP_SLOT_SIZE;
}

/* 对象地址 → 宿主记录（非池内对象/未初始化返回 NULL） */
static zm_img_rec *rec_of(uint32_t obj) {
  if (obj >= IMAGE_POOL && obj < IMAGE_POOL + IMAGE_SLOT_COUNT * IMAGE_SLOT_SIZE)
    return &g_img[(obj - IMAGE_POOL) / IMAGE_SLOT_SIZE];
  if (obj >= BITMAP_POOL &&
      obj < BITMAP_POOL + BITMAP_SLOT_COUNT * BITMAP_SLOT_SIZE)
    return &g_bmp[(obj - BITMAP_POOL) / BITMAP_SLOT_SIZE];
  if (obj == BITMAP)
    return &g_bmp_single;
  return NULL;
}

/* 把任意对象地址折算到"持有像素"的记录：
 * entry 本身不持像素，它 +8 的 SURF 才持像素（见 emu.h 说明）。 */
/* 解码像素池：循环复用。IImage 解码出的像素必须落在客户机内存，
 * applet 自带 GDI 是按 IBitmap 的 +36 像素指针直接读的。 */
static uint32_t s_pix_next = 0;
uint32_t zm_pix_pool_alloc(uint32_t bytes) {
  bytes = (bytes + 3u) & ~3u;
  if (!bytes || bytes > PIX_POOL_SIZE)
    return 0;
  if (s_pix_next + bytes > PIX_POOL_SIZE)
    s_pix_next = 0; /* 回绕复用 */
  uint32_t p = PIX_POOL + s_pix_next;
  s_pix_next += bytes;
  return p;
}

static zm_img_rec *pixel_rec(uint32_t obj) {
  zm_img_rec *r = rec_of(obj);
  if (r && r->kind == REC_ENTRY)
    r = rec_of(obj + IMAGE_ENTRY_OFF_SURF);
  return r;
}

static void rec_free_pixels(zm_img_rec *r) {
  if (r->rgba) {
    free(r->rgba);
    r->rgba = NULL;
  }
  r->w = r->h = 0;
}

void zm_image_reset(void) {
  for (int i = 0; i < IMAGE_SLOT_COUNT; i++) {
    rec_free_pixels(&g_img[i]);
    memset(&g_img[i], 0, sizeof(g_img[i]));
  }
  for (int i = 0; i < BITMAP_SLOT_COUNT; i++) {
    rec_free_pixels(&g_bmp[i]);
    memset(&g_bmp[i], 0, sizeof(g_bmp[i]));
  }
  rec_free_pixels(&g_bmp_single);
  memset(&g_bmp_single, 0, sizeof(g_bmp_single));
}

/* 申请一个空闲槽；没有空闲则回收引用计数为 0 的槽位 */
static int pool_claim(zm_img_rec *pool, int count, rec_kind kind) {
  for (int i = 0; i < count; i++) {
    if (!pool[i].used) {
      rec_free_pixels(&pool[i]);
      memset(&pool[i], 0, sizeof(pool[i]));
      pool[i].used = 1;
      pool[i].kind = kind;
      pool[i].refcnt = 1;
      return i;
    }
  }
  for (int i = 0; i < count; i++) {
    if (pool[i].refcnt <= 0) {
      rec_free_pixels(&pool[i]);
      memset(&pool[i], 0, sizeof(pool[i]));
      pool[i].used = 1;
      pool[i].kind = kind;
      pool[i].refcnt = 1;
      log_debug("zm_image: 图像池满，回收槽 %d（%s）", i, pool[i].name);
      return i;
    }
  }
  return -1;
}

/* =========================================================================
 * PNG / JPEG 解码（统一产出 RGBA8888）
 * ========================================================================= */

static int decode_png(const uint8_t *data, size_t len, int *ow, int *oh,
                      uint8_t **orgba) {
  png_image img;
  memset(&img, 0, sizeof(img));
  img.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&img, data, len)) {
    log_warn("PNG 解析失败: %s", img.message);
    return -1;
  }
  img.format = PNG_FORMAT_RGBA;
  size_t sz = PNG_IMAGE_SIZE(img);
  uint8_t *buf = malloc(sz ? sz : 1);
  if (!buf) {
    png_image_free(&img);
    return -1;
  }
  if (!png_image_finish_read(&img, NULL, buf, 0, NULL)) {
    log_warn("PNG 解码失败: %s", img.message);
    free(buf);
    png_image_free(&img);
    return -1;
  }
  *ow = (int)img.width;
  *oh = (int)img.height;
  *orgba = buf;
  png_image_free(&img);
  return 0;
}

struct zm_jpg_err {
  struct jpeg_error_mgr pub;
  jmp_buf jump;
};

static void zm_jpg_error_exit(j_common_ptr cinfo) {
  struct zm_jpg_err *e = (struct zm_jpg_err *)cinfo->err;
  longjmp(e->jump, 1);
}

static int decode_jpg(const uint8_t *data, size_t len, int *ow, int *oh,
                      uint8_t **orgba) {
  struct jpeg_decompress_struct cinfo;
  struct zm_jpg_err jerr;
  memset(&cinfo, 0, sizeof(cinfo));
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = zm_jpg_error_exit;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_decompress(&cinfo);
    log_warn("JPEG 解码失败");
    return -1;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, data, (unsigned long)len);
  jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);

  int w = (int)cinfo.output_width;
  int h = (int)cinfo.output_height;
  uint8_t *rgba = malloc((size_t)w * h * 4u);
  if (!rgba) {
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }
  size_t row = (size_t)w * cinfo.output_components;
  uint8_t *line = malloc(row);
  if (!line) {
    free(rgba);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW rows[1] = {line};
    jpeg_read_scanlines(&cinfo, rows, 1);
    uint8_t *dst = rgba + (size_t)(cinfo.output_scanline - 1) * w * 4u;
    for (int x = 0; x < w; x++) {
      dst[x * 4 + 0] = line[x * 3 + 0];
      dst[x * 4 + 1] = line[x * 3 + 1];
      dst[x * 4 + 2] = line[x * 3 + 2];
      dst[x * 4 + 3] = 0xFF;
    }
  }
  free(line);
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  *ow = w;
  *oh = h;
  *orgba = rgba;
  return 0;
}

/* 按内容魔数选择解码器（applet 传的文件名可能没有扩展名） */
static int decode_any(const uint8_t *data, size_t len, int *w, int *h,
                      uint8_t **rgba) {
  if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
      data[3] == 'G')
    return decode_png(data, len, w, h, rgba);
  if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
    return decode_jpg(data, len, w, h, rgba);
  /* 兜底：先按 PNG，再按 JPEG 试一遍（某些文件头带偏移） */
  if (decode_png(data, len, w, h, rgba) == 0)
    return 0;
  return decode_jpg(data, len, w, h, rgba);
}

/* 读文件 + 解码，填入记录；成功返回 0 */
static int rec_load_file(zm_img_rec *r, const char *name) {
  uint8_t *raw = NULL;
  size_t raw_len = 0;
  if (zm_fs_read_file(name, &raw, &raw_len) != 0)
    return -1;

  int w = 0, h = 0;
  uint8_t *rgba = NULL;
  int rc = decode_any(raw, raw_len, &w, &h, &rgba);
  free(raw);
  if (rc != 0) {
    log_warn("IImage: 无法解码 \"%s\"（%zu 字节）", name, raw_len);
    return -1;
  }
  rec_free_pixels(r);
  r->w = w;
  r->h = h;
  r->rgba = rgba;
  snprintf(r->name, sizeof(r->name), "%s", name);
  log_info("IImage: 载入 \"%s\" %dx%d", name, w, h);
  return 0;
}

/* =========================================================================
 * IImage 槽实现
 * ========================================================================= */

uint32_t zm_image_CreateImage(uc_engine *uc, uint32_t r0, uint32_t r1,
                              uint32_t r2, uint32_t r3) {
  (void)r0; /* this = display，未用 */
  (void)r1; /* alloc 回调（真实固件用它分配解码缓冲） */
  (void)r2; /* free 回调 */
  uint32_t out_ptr = r3;

  /* entry 与它的 surface（entry+8）各占一个连续槽：申请 i 与 i+1 */
  int idx = -1;
  for (int i = 0; i + 1 < IMAGE_SLOT_COUNT; i += 2) {
    if (!g_img[i].used && !g_img[i + 1].used) {
      rec_free_pixels(&g_img[i]);
      memset(&g_img[i], 0, sizeof(g_img[i]));
      g_img[i].used = 1;
      g_img[i].kind = REC_ENTRY;
      g_img[i].refcnt = 1;

      rec_free_pixels(&g_img[i + 1]);
      memset(&g_img[i + 1], 0, sizeof(g_img[i + 1]));
      g_img[i + 1].used = 1;
      g_img[i + 1].kind = REC_SURF;
      g_img[i + 1].refcnt = 1;
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    log_error("IDisplay::CreateImage 失败：图像对象池已满");
    if (out_ptr)
      uc_write32(uc, out_ptr, 0);
    return (uint32_t)-1;
  }

  uint32_t obj = img_addr(idx);
  uint32_t surf = img_addr(idx + 1);

  /* entry：首字=entry 虚表；+4=逐帧偏移表（applet 会读，先给 0）；
   * +8=surface 对象地址（**BitBlt 传的就是这个**，逆向 sub_37B4）。
   * 写入 entry+8 后，BitBlt 的 surface 参数即 surf。 */
  uc_write32(uc, obj, IMAGE_VT);
  uc_write32(uc, obj + 4, 0);
  uc_write32(uc, obj + 8, surf);

  /* surface：首字=自己的虚表（对象池里仍按 surf 识别）；其余为解码元数据 */
  uc_write32(uc, surf, IMAGE_VT);

  if (out_ptr)
    uc_write32(uc, out_ptr, obj);
  log_debug("IDisplay::CreateImage -> entry@0x%X surf@0x%X (slot %d)", obj,
            surf, idx);
  return 0; /* 0 = 成功 */
}

/* 供 applet 的"先建空 surface 再自己解码填像素"路径（sub_379C）使用：
 * 若 surf 槽尚无像素，视为已有解码数据挂在 surf 上（真机行为）；这里只
 * 保证 surf 地址合法（CreateImage 时已构造）。 */
int zm_image_surf_ready(uint32_t surf) {
  zm_img_rec *r = rec_of(surf);
  return (r && r->kind == REC_SURF && r->rgba) ? 1 : 0;
}

uint32_t zm_image_AddRef(uc_engine *uc, uint32_t r0) {
  (void)uc;
  zm_img_rec *r = rec_of(r0);
  if (r)
    r->refcnt++;
  return (uint32_t)(r ? r->refcnt : 0);
}

uint32_t zm_image_Release(uc_engine *uc, uint32_t r0) {
  (void)uc;
  zm_img_rec *r = rec_of(r0);
  if (!r)
    return 0;
  r->refcnt--;
  if (r->refcnt <= 0) {
    rec_free_pixels(r);
    r->used = 0;
  }
  return 0;
}

/* +0x08 SetData(this, mode, name_ptr, len)
 * applet 传的是**文件名**（sprintf 拼出的 "res\xxx.png"），len = strlen。
 * 返回 0 = 成功；非 0 = 失败（applet 会立刻 Release）。 */
uint32_t zm_image_SetData(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                          uint32_t r3) {
  (void)r1; /* mode/flag，实测恒为 0 */
  /* 像素挂在 entry+8 的 surface 上，entry 自身不持像素 */
  zm_img_rec *r = pixel_rec(r0);
  if (!r)
    return (uint32_t)-1;

  char name[256];
  read_cstr(uc, r2, name, sizeof(name));
  if (name[0] == '\0' && r3)
    return (uint32_t)-1;
  return rec_load_file(r, name) == 0 ? 0u : (uint32_t)-1;
}

uint32_t zm_image_x0C(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 0; /* GetType：JPEG/PNG 类型枚举，applet 未据此分支 */
}

uint32_t zm_image_x10(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  /* applet 的"解码前准备"（vt+0x10），无返回值依赖 */
  return 0;
}

uint32_t zm_image_x14(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 0; /* vt+0x14：帧数/清理，未依赖 */
}

uint32_t zm_image_Width(uc_engine *uc, uint32_t r0) {
  (void)uc;
  zm_img_rec *r = pixel_rec(r0);
  return (uint32_t)(r ? r->w : 0);
}

/* +0x1C Decode(this=entry, alloc=r1, free=r2, out=r3, flags=[sp+0]) → 0 成功
 *
 * 逆向（00000506 sub_3644 → sub_37B4 → vt[0x1C]）：
 *   1) CreateImage(disp, sub_7E54, sub_7E50, &entry)  ← r1/r2 是 applet 的
 *      分配/释放回调；entry[0xC] 是喂进 IImage 的**原始数据 blob**。
 *   2) entry->vt[0x1C](entry, sub_7E54, sub_7E50, &rec, 0)  ← 再次把回调传进来
 *   3) 之后 rec+0xC 被当对象，调它的 vt[4]/vt[8] 取宽高，再传给 BitBlt。
 * 也就是说：**解码产物是 applet 自己用回调分配出来的**，模拟器直接
 * 构造一个"门面"并写进 out 即可。这里把门面指向 entry+8 的 surface
 * （像素挂在它上面），并把宽高同时写进 out+8/+0xC 与门面自身，
 * 这样无论 applet 走 vt[4]/vt[8] 还是裸读字段都能取到正确值。
 *
 * 注意：旧实现忽略了 r1/r2（applet 的分配回调），导致 applet 侧
 * 自己算出的记录里仍是脏指针（0x823200 这种栈地址），
 * DrawBitmapEx 随即失败。此处保持"由模拟器提供对象"的策略。 */
uint32_t zm_image_DecodeToBitmap(uc_engine *uc, uint32_t r0, uint32_t r1,
                                 uint32_t r2, uint32_t r3, uint32_t sp) {
  (void)r2;
  (void)sp;
  zm_img_rec *src = pixel_rec(r0);
  uint32_t out_ptr = r3;
  if (!src || !src->rgba) {
    if (out_ptr)
      uc_write32(uc, out_ptr, 0);
    log_warn("IImage::Decode 失败：entry 0x%X 尚未装入图像数据", r0);
    return (uint32_t)-1;
  }

  /* 再申请一个独立对象当"解码产物"（与 entry+8 的 surface 同型），
   * 避免与 entry 自身的 surface 别名混淆；宽高写在门面 +8/+0xC。 */
  int idx = pool_claim(g_img, IMAGE_SLOT_COUNT, REC_SURF);
  if (idx < 0) {
    if (out_ptr)
      uc_write32(uc, out_ptr, 0);
    return (uint32_t)-1;
  }
  zm_img_rec *surf = &g_img[idx];
  surf->w = src->w;
  surf->h = src->h;
  size_t sz = (size_t)src->w * (size_t)src->h * 4u;
  surf->rgba = malloc(sz ? sz : 1);
  if (surf->rgba && sz)
    memcpy(surf->rgba, src->rgba, sz);
  snprintf(surf->name, sizeof(surf->name), "%s", src->name);
  uint32_t surf_obj = img_addr(idx);
  /* **必须写对象首字**：zmaee 里对象首字是虚表指针。
   * 漏写会导致调用点 `ldr r1,[obj]; ldr r1,[r1,#4]; blx r1` 读到
   * `*(0+4)`（即 payload 低地址的垃圾），实测会跳进 0x4/0x8/0xC...
   * 逐条执行到非法指令而崩溃。 */
  uc_write32(uc, surf_obj, SURF_VT);

  /* ---- 在客户机内存里把 surf_obj 建成一个合法 IBitmap ----
   * RE：ZMAEE_IBitmap_GetInfo(bitmap, out) 就是 `memcpy(out, bitmap + 8, 32)`，
   * 于是 out 的 8 个 dword 恰好是 IBitmap 的字段：
   *   +8 宽   +12 高   +16 颜色格式   +20 透明色
   *   +24 调色板标志   +28 调色板指针   +32 调色板大小   +36 像素指针
   * applet 自带的 GDI 直接按 +36 去读像素 —— 宿主侧那份 rgba 它看不到，
   * 所以像素必须拷进客户机，并转成 ARGB8888 以保留逐像素 alpha
   * （RE：ZMAEE_Copy32To16 用 `v6 >> 27` 取 alpha，0 则整像素跳过）。 */
  {
    uint32_t gpx = 0;
    if (sz)
      gpx = zm_pix_pool_alloc((uint32_t)sz);
    if (gpx && src->rgba) {
      static uint32_t line[512];
      for (int yy = 0; yy < src->h; yy++) {
        const uint8_t *sp = src->rgba + (size_t)yy * (size_t)src->w * 4u;
        int done = 0;
        while (done < src->w) {
          int n = src->w - done;
          if (n > 512)
            n = 512;
          for (int i = 0; i < n; i++) {
            const uint8_t *q = sp + (size_t)(done + i) * 4u;
            uint32_t v = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                         ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
            /* RGBA8888 → ARGB8888 */
            line[i] = ((v & 0xFFu) << 16) | (((v >> 8) & 0xFFu) << 8) |
                      ((v >> 16) & 0xFFu) | (v & 0xFF000000u);
          }
          uc_mem_write(uc, gpx + (uint32_t)yy * (uint32_t)src->w * 4u +
                               (uint32_t)done * 4u, line, (size_t)n * 4u);
          done += n;
        }
      }
    }
    uc_write32(uc, surf_obj + 4, 1);  /* 引用计数 */
    uc_write32(uc, surf_obj + 8, (uint32_t)src->w);
    uc_write32(uc, surf_obj + 12, (uint32_t)src->h);
    uc_write32(uc, surf_obj + 16, 2); /* 颜色格式：2 = 32bit ARGB8888 */
    uc_write32(uc, surf_obj + 20, 0xFFFFFFFFu); /* 透明色 -1 → 用 alpha 通道 */
    uc_write32(uc, surf_obj + 24, 0);           /* 无调色板 */
    uc_write32(uc, surf_obj + 28, 0);
    uc_write32(uc, surf_obj + 32, 0);
    uc_write32(uc, surf_obj + 36, gpx); /* 像素指针 */
    uc_write32(uc, surf_obj + 40, 0);
    log_debug("IImage::Decode -> IBitmap@0x%X %dx%d fmt=32bit pix@0x%X", surf_obj,
              src->w, src->h, gpx);
  }

  /* entry+8 也指向它，保证 BitBlt 拿 entry+8 时同样有效 */
  if (rec_of(r0) && rec_of(r0)->kind == REC_ENTRY)
    uc_write32(uc, r0 + IMAGE_ENTRY_OFF_SURF, surf_obj);

  if (out_ptr) {
    /* out 是"解码结果记录"。applet 的消费方式（sub_313C @0x3278）：
     *     sub_27C(0, out[0], R11)      → 造一个 16B 绘制负载
     *     负载 = { vt=unk_1A9F0, 1, out[0], R11 }
     * 之后绘制分派把负载的 +0xC 当图像对象交给 DrawBitmapEx/BitBlt。
     * 也就是说 **out[0] 必须是"带像素的图像对象指针"**（真机是解码出的
     * 堆对象）。旧实现把 out[0] 写成虚表地址 SURF_VT，
     * 于是每次绘制都拿到 0x7A3200（虚表本身）→ 找不到像素 → 733 次失败。
     * 这里写入 surf_obj（对象首字即 SURF_VT，可正常响应 vt 调用）。 */
    uc_write32(uc, out_ptr, surf_obj);
    uc_write32(uc, out_ptr + 4, 0);
    uc_write32(uc, out_ptr + 8, (uint32_t)src->w);
    uc_write32(uc, out_ptr + 0x0C, (uint32_t)src->h);
  }
  log_debug("IImage::Decode -> surf=0x%X %dx%d (out@0x%X)", surf_obj, src->w,
            src->h, out_ptr);
  return 0;
}

/* ---- surface 门面（Decode 的 out / entry+8）---- */

uint32_t zm_surf_addRef(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 1;
}

uint32_t zm_surf_release(uc_engine *uc, uint32_t r0) {
  (void)uc;
  /* surface 的像素由 entry 持有，随 entry 一起回收（Release 走图像池） */
  zm_img_rec *r = rec_of(r0);
  if (r && r->kind == REC_SURF && r->refcnt > 0)
    r->refcnt--;
  return 0;
}

uint32_t zm_surf_wh(uc_engine *uc, uint32_t r0, uint32_t which) {
  (void)uc;
  zm_img_rec *r = pixel_rec(r0);
  if (!r)
    return 0;
  return (uint32_t)(which == 0 ? r->w : r->h);
}

/* SURF_VT+0x10：GetRect(this, out) —— 写矩形 {left, top, right, bottom}。
 * 逆向（00000506 sub_388 type1 → loc_448）：
 *   obj = res+0xC; obj->vt[0x10](obj, &var_30);
 *   var_8 = var_30; var_4 = var_2C;      ← 只取前两个字段
 *   DrawBitmapEx(disp, x, y, obj, {var_8, var_4, 0, 0}, res+8, 0);
 * 用 int16 写，兼顾"矩形"与"宽高对"两种解释。 */
uint32_t zm_surf_getrect(uc_engine *uc, uint32_t r0, uint32_t r1) {
  zm_img_rec *r = pixel_rec(r0);
  int w = r ? r->w : 0;
  int h = r ? r->h : 0;
  if (r1) {
    int16_t rect[4] = {0, 0, (int16_t)w, (int16_t)h};
    uc_mem_write(uc, r1, rect, sizeof(rect));
  }
  return 0;
}

uint32_t zm_surf_encode(uc_engine *uc, uint32_t r0, uint32_t r1,
                        uint32_t r2, uint32_t r3) {
  (void)uc;
  (void)r0;
  (void)r1;
  (void)r2;
  (void)r3;
  log_debug("surface::Encode stub");
  return 0;
}

uint32_t zm_surf_nop(uc_engine *uc, uint32_t off) {
  (void)uc;
  (void)off;
  return 0;
}

uint32_t zm_image_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                       uint32_t r2, uint32_t r3) {
  (void)uc;
  (void)r0;
  (void)r1;
  (void)r2;
  (void)r3;
  log_debug("image stub[0x%X]", off);
  return 0;
}

/* =========================================================================
 * 供 IDisplay 取像素
 * ========================================================================= */

/* =========================================================================
 * ZMAEE_GDI_Surface 支持（见 zm_image.h 的布局说明）
 * ========================================================================= */

int zm_image_get_gdi_surface(uc_engine *uc, uint32_t surf, int *w, int *h,
                             int *step_out, uint32_t *colorkey) {
  uint32_t v[4];
  if (uc_mem_read(uc, surf, v, sizeof(v)) != UC_ERR_OK)
    return 0;

  int gw = (int)v[0], gh = (int)v[1];
  int gs = (int)v[2]; /* 每像素字节数（GDI_Ext 用 byte_5B658 得出的步进） */
  /* 合法性判据（防止把普通指针/字符串误认成 surface）：
   * 尺寸在屏幕范围附近、步进是 1/2/3/4 之一。 */
  if (gw <= 0 || gh <= 0 || gw > 4096 || gh > 4096)
    return 0;
  if (gs < 1 || gs > 4)
    return 0;

  if (w)
    *w = gw;
  if (h)
    *h = gh;
  if (step_out)
    *step_out = gs;
  /* surf[3]：Mask16To16 里 *(*(a4+12)+12) 是透明色；
   * 本模拟器直接按"透明色 / 调色板对象指针"两种语义都接受。 */
  if (colorkey)
    *colorkey = v[3];
  return 1;
}

/* 读调色板对象（仅 8bpp 索引色路径用）：
 * *trans_idx = 透明色索引（0..255），*pal = RGB565[256] 基址 */
static int gdi_read_palette(uc_engine *uc, uint32_t pal_obj, int *trans_idx,
                            uint32_t *pal) {
  uint32_t vi = 0, vp = 0;
  if (uc_mem_read(uc, pal_obj + 12, &vi, 4) != UC_ERR_OK)
    return 0;
  if (uc_mem_read(uc, pal_obj + 20, &vp, 4) != UC_ERR_OK)
    return 0;
  if (vi > 255)
    return 0;
  if (trans_idx)
    *trans_idx = (int)vi;
  if (pal)
    *pal = vp;
  return 1;
}

/* 按 surface 的**格式码**把一个像素读成 ARGB8888。
 *
 * 格式码 = surf[8]，与 ZMAEE_GDI_BitBlt_Ext 的 switch 分支一一对应
 * （case 1 → mask16/copy16，case 2 → 24，case 3 → 32，case 4 → P32），
 * 即它就是"像素格式"，而 byte_5B658[fmt] 给出的是**每像素字节数**：
 *
 *   fmt 1 = RGB565   （2 字节；GDI_Ext 走 mask16/copy16）
 *   fmt 2 = RGB888   （3 字节；mask24/copy24）
 *   fmt 3 = 32bpp    （4 字节；mask32/copy32，0x00RRGGBB）
 *   fmt 4 = P32      （4 字节，带 alpha；maskP32/copyP32）
 *
 * 实测验证：背景 surface {240,320,1,0} 的数据是 0x22D1/0x22D0（RGB565 深蓝灰），
 * 精灵 surface {49,26,3,0xF81F} 的数据是 0x00292828（32bpp 深灰）——
 * 与上述映射完全吻合。**注意 fmt 1 不是 8bpp 索引色**，早期按索引色处理
 * 会把 RGB565 的高低字节拆成两个像素，画面呈细密噪点。
 *
 * surf[0xC] 在这里按"透明色"解释（精灵实测 0xF81F = RGB565 洋红）：
 * 比较时按当前格式截断。返回 0 表示透明（跳过），-1 读失败。 */
static int gdi_read_pixel(uc_engine *uc, uint32_t addr, int fmt, uint32_t ck,
                          int *out_argb) {
  if (fmt == 1) { /* RGB565 */
    uint16_t v = 0;
    if (uc_mem_read(uc, addr, &v, 2) != UC_ERR_OK)
      return -1;
    if ((uint32_t)v == (ck & 0xFFFFu))
      return 0; /* 透明色（Mask16 语义） */
    unsigned r = ((v >> 11) & 0x1F) * 255 / 31;
    unsigned g = ((v >> 5) & 0x3F) * 255 / 63;
    unsigned b = (v & 0x1F) * 255 / 31;
    *out_argb = (int)(0xFF000000u | (r << 16) | (g << 8) | b);
    return 1;
  }
  if (fmt == 2) { /* RGB888 */
    uint8_t p[3];
    if (uc_mem_read(uc, addr, p, 3) != UC_ERR_OK)
      return -1;
    uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    if (v == (ck & 0xFFFFFFu))
      return 0;
    *out_argb = (int)(0xFF000000u | v);
    return 1;
  }
  /* fmt 3 = 32bpp（0x00RRGGBB）；fmt 4 = P32（含 alpha，0xAARRGGBB） */
  uint32_t v = 0;
  if (uc_mem_read(uc, addr, &v, 4) != UC_ERR_OK)
    return -1;
  if ((v & 0xFFFFFFu) == (ck & 0xFFFFFFu))
    return 0;
  if (fmt == 4 && (v >> 24))
    *out_argb = (int)v;
  else
    *out_argb = (int)(0xFF000000u | (v & 0xFFFFFFu));
  return 1;
}

int zm_image_blit_gdi_surface(uc_engine *uc, uint32_t surf, int dx, int dy,
                              uint32_t rect_ptr, int mode) {
  int w = 0, h = 0, step = 0;
  uint32_t ck = 0;
  if (!zm_image_get_gdi_surface(uc, surf, &w, &h, &step, &ck))
    return 0;

  int sx = 0, sy = 0, sw = w, sh = h;
  if (rect_ptr) {
    int l = (int)uc_read32(uc, rect_ptr);
    int t = (int)uc_read32(uc, rect_ptr + 4);
    int r = (int)uc_read32(uc, rect_ptr + 8);
    int b = (int)uc_read32(uc, rect_ptr + 12);
    if (r > l && b > t) {
      sx = l;
      sy = t;
      sw = r - l;
      sh = b - t;
    }
  }
  if (sx < 0 || sy < 0 || sx + sw > w || sy + sh > h || sw <= 0 || sh <= 0)
    return 0;

  /* 格式码 → 每像素字节数（见 gdi_read_pixel 的说明） */
  int bytes = (step == 1) ? 2 : (step == 2 ? 3 : 4);
  /* 像素基址候选，按优先级：
   *   1) *(surf+0x1C) —— applet 在 sub_10248 里把 layer_info[0x24]（层
   *      像素缓冲指针）写进 surf+0x1C，这是**指针**，指向真正的像素数据；
   *   2) surf+0x1C 本身 —— 兼容"结构头 + 紧随其后的内联像素"布局；
   *   3) *(surf+8) —— 少数对象把数据指针放在 +8。
   * 取第一个"末像素可读"的候选。 */
  uint32_t base_cand[3];
  int ncand = 0;
  uint32_t dp = 0;
  if (uc_mem_read(uc, surf + GDI_SURFACE_HDR, &dp, 4) == UC_ERR_OK &&
      dp >= 0x1000u && dp < 0xFFF00000u)
    base_cand[ncand++] = dp;
  base_cand[ncand++] = surf + GDI_SURFACE_HDR;
  if (uc_mem_read(uc, surf + 8, &dp, 4) == UC_ERR_OK && dp >= 0x1000u &&
      dp < 0xFFF00000u)
    base_cand[ncand++] = dp;

  uint32_t base = 0;
  for (int i = 0; i < ncand; i++) {
    uint8_t probe[4] = {0};
    uint32_t last = base_cand[i] + ((size_t)w * h - 1u) * (size_t)bytes;
    if (uc_mem_read(uc, last, probe, bytes > 4 ? 4 : bytes) == UC_ERR_OK) {
      base = base_cand[i];
      break;
    }
  }
  if (!base)
    return 0; /* 候选基址都不可读 → 不是有效 surface */

  if (g_disasm) {
    static int shown = 0;
    if (shown < 12) {
      uint16_t d[4] = {0, 0, 0, 0};
      uc_mem_read(uc, base, d, sizeof(d));
      log_debug("GDI blit: surf=0x%X w=%d h=%d step=%d ck=0x%X | "
                "*(surf+0x1C)=0x%X base=0x%X 数据[%04X %04X %04X %04X]",
                surf, w, h, step, ck, uc_read32(uc, surf + 0x1C), base, d[0],
                d[1], d[2], d[3]);
      shown++;
    }
  }

  if (!zm_fb_buffer(NULL, NULL))
    return 1; /* 无显示后端：已识别为合法 surface，只是无处可画 */

  for (int y = 0; y < sh; y++) {
    for (int x = 0; x < sw; x++) {
      int argb = 0;
      int rc =
          gdi_read_pixel(uc, base + ((size_t)(sy + y) * w + (sx + x)) * bytes,
                         step, ck, &argb);
      if (rc <= 0)
        continue; /* 透明/越界 */
      int ddx = x, ddy = y;
      switch (mode & 7) {
      case 1: ddy = sh - 1 - y; break;
      case 2: ddx = sw - 1 - x; break;
      case 3: ddx = sw - 1 - x; ddy = sh - 1 - y; break;
      case 4: ddx = y; ddy = x; break;
      case 5: ddx = sh - 1 - y; ddy = x; break;
      case 6: ddx = y; ddy = sw - 1 - x; break;
      case 7: ddx = sh - 1 - y; ddy = sw - 1 - x; break;
      default: break;
      }
      zm_fb_write(dx + ddx, dy + ddy, (uint32_t)argb);
    }
  }
  return 1;
}

int zm_image_get_pixels(uint32_t obj, int *w, int *h, const uint8_t **rgba) {
  /* entry / surf / bitmap 都能直接取到像素：entry 自动折到它 +8 的 surf */
  zm_img_rec *r = pixel_rec(obj);
  if (!r || !r->rgba || r->w <= 0 || r->h <= 0)
    return 0;
  if (w)
    *w = r->w;
  if (h)
    *h = r->h;
  if (rgba)
    *rgba = r->rgba;
  return 1;
}

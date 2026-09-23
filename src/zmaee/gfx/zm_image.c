#include "zm_image.h"

#include "../../emu.h"
#include "../../log/log.h"
#include "../../tool/uc_helper.h"
#include "../core/zm_str.h"
#include "../fs/zm_file_mgr.h"
#include "zm_display.h" /* 软件帧缓冲：贴 GDI_Surface 用 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_image.h> /* PNG/JPG 解码：IMG_Load_RW（自带 libpng/libjpeg） */

/* =========================================================================
 * IImage / IBitmap 对象池
 * ========================================================================= */

typedef enum { REC_ENTRY = 0, REC_SURF = 1, REC_BITMAP = 2 } rec_kind;

/* IImage 的**类型枚举** —— 存在对象 +0x18，是 ZMAEE_IImage_Decode 的分派键。
 * RE（ZMAEE_IImage_Decode 反编译）：
 *     v11 = a1[6];                     // = *(int *)(this + 0x18)
 *     if      (v11 == 1) PNG_Decode(); // 1 = PNG
 *     else if (v11 == 2) JPG_Decode(); // 2 = JPG
 *     else if (v11 != 0) return -1;    // 其它值 = 非法类型
 *     else               GIF_Decode(); // 0 = GIF（也是对象刚建好时的默认值）
 * 所以"恒填 0"等价于把所有图都标成 GIF —— 必须按实际文件格式回填。 */
enum { ZM_IMG_TYPE_GIF = 0, ZM_IMG_TYPE_PNG = 1, ZM_IMG_TYPE_JPG = 2 };

typedef struct {
  int used;
  rec_kind kind;   /* ENTRY=CreateImage 造的数据对象；SURF=entry+8 绘制对象；
                      BITMAP=Decode 出的位图 */
  int refcnt;
  int w, h;
  int type;        /* ZM_IMG_TYPE_*：装入时按魔数定，写回 entry +0x18 */
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
  if (obj == G_BITMAP_ADDR)
    return &g_bmp_single;
  return NULL;
}

/* 把任意对象地址折算到"持有像素"的记录：
 * entry 本身不持像素，它 +8 的 SURF 才持像素（见 emu.h 说明）。 */
/* 解码像素池：循环复用。IImage 解码出的像素必须落在客户机内存，
 * applet 自带 GDI 是按 IBitmap 的 +36 像素指针直接读的。 */
static uint32_t s_pix_next = 0;
/* bump 指针的上限。默认是整个池；被 zm_pix_pool_reserve_tail 从尾部预留
 * 常驻缓冲后会相应压低，从而保证回绕复用不会踩到常驻缓冲。 */
static uint32_t s_pix_limit = PIX_POOL_SIZE;

uint32_t zm_pix_pool_alloc(uint32_t bytes) {
  bytes = (bytes + 3u) & ~3u;
  if (!bytes || bytes > s_pix_limit)
    return 0;
  if (s_pix_next + bytes > s_pix_limit)
    s_pix_next = 0; /* 回绕复用 */
  uint32_t p = PIX_POOL + s_pix_next;
  s_pix_next += bytes;
  return p;
}

uint32_t zm_pix_pool_reserve_tail(uint32_t bytes) {
  bytes = (bytes + 3u) & ~3u;
  if (!bytes || bytes > s_pix_limit)
    return 0;
  uint32_t base = PIX_POOL + (s_pix_limit - bytes);
  s_pix_limit -= bytes;
  /* 防御：万一调用时机晚于某些分配，把它们一起作废，避免越界 */
  if (s_pix_next > s_pix_limit)
    s_pix_next = 0;
  return base;
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

void zm_image_dump_pool(void) {
  int n_img = 0, n_bmp = 0;
  for (int i = 0; i < IMAGE_SLOT_COUNT; i++)
    if (g_img[i].used)
      n_img++;
  for (int i = 0; i < BITMAP_SLOT_COUNT; i++)
    if (g_bmp[i].used)
      n_bmp++;
  log_info("  [图像池] IImage 在用 %d 个：", n_img);
  for (int i = 0; i < IMAGE_SLOT_COUNT; i++) {
    if (!g_img[i].used)
      continue;
    log_info("     槽%-3d %-5s %4dx%-4d ref=%d obj=0x%X \"%s\"", i,
             g_img[i].kind == REC_ENTRY
                 ? "entry"
                 : (g_img[i].kind == REC_SURF ? "surf" : "?"),
             g_img[i].w, g_img[i].h, g_img[i].refcnt, img_addr(i),
             g_img[i].name);
  }
  log_info("  [图像池] IBitmap 在用 %d 个：", n_bmp);
  for (int i = 0; i < BITMAP_SLOT_COUNT; i++) {
    if (!g_bmp[i].used)
      continue;
    log_info("     槽%-3d %-5s %4dx%-4d ref=%d obj=0x%X \"%s\"", i,
             g_bmp[i].kind == REC_ENTRY
                 ? "entry"
                 : (g_bmp[i].kind == REC_SURF ? "surf" : "?"),
             g_bmp[i].w, g_bmp[i].h, g_bmp[i].refcnt, bmp_addr(i),
             g_bmp[i].name);
  }
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
 *
 * 但发给客户机的 IBitmap 必须带**真实的颜色格式与透明色**：
 * RE：ZMAEE_IImage_PNG_Decode 里
 *   v72 = PNG color type
 *     3(调色板) → format 0 ；2(truecolor) → format 2 ；6(RGBA) → format 3
 *   v75 = -1（默认不透明），若存在 tRNS 块则取其中的透明色，
 *   最后 ZMAEE_IBitmap_SetTransColor(bitmap, v75)。
 * 以前我们统一写 format=2 / transcolor=-1，与固件不符。
 * ========================================================================= */
static int g_last_fmt = 2;       /* 0=8bit索引 2=RGB 3=RGBA */
static uint32_t g_last_tc = 0xFFFFFFFFu; /* 透明色，-1 = 不透明 */

/* 大端 16 位读 */
static uint32_t be16(const uint8_t *p) {
  return ((uint32_t)p[0] << 8) | p[1];
}
static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
         p[3];
}

/* 从 PNG 原始字节里取 color type 与 tRNS 透明色（有 tRNS 时才覆盖 *tc） */
static void png_meta(const uint8_t *d, size_t len, int *fmt, uint32_t *tc) {
  *fmt = 2;
  if (len < 26)
    return;
  int ctype = d[25]; /* 8(签名) + 4(长度) + 4("IHDR") + 9 = 25 */
  switch (ctype) {
  case 3:
    *fmt = 0; /* 调色板：固件用 format 0（带调色板） */
    break;
  case 2:
    *fmt = 2;
    break;
  case 6:
    *fmt = 3; /* RGBA：固件用 format 3 */
    break;
  case 0:
    *fmt = 2; /* 灰度按 RGB 处理 */
    break;
  case 4:
    *fmt = 3; /* 灰度+alpha */
    break;
  default:
    *fmt = 2;
    break;
  }
  /* 扫块找 tRNS（到 IDAT 为止） */
  size_t off = 8;
  while (off + 12 <= len) {
    uint32_t clen = be32(d + off);
    const uint8_t *ctype4 = d + off + 4;
    if (memcmp(ctype4, "IDAT", 4) == 0 || memcmp(ctype4, "IEND", 4) == 0)
      break;
    if (memcmp(ctype4, "tRNS", 4) == 0) {
      const uint8_t *t = d + off + 8;
      if (off + 8 + clen > len)
        break;
      if (ctype == 2 && clen >= 6) {
        /* truecolor：6 字节 = R,G,B（各 16 位大端，值域同 8 位） */
        uint32_t r = be16(t) & 0xFFu;
        uint32_t g = be16(t + 2) & 0xFFu;
        uint32_t b = be16(t + 4) & 0xFFu;
        *tc = (r << 16) | (g << 8) | b; /* alpha=0 */
      }
      /* 调色板(3)/灰度(0) 的 tRNS 由 libpng 展开进 alpha，这里不额外处理 */
      break;
    }
    off += 12u + clen;
  }
}

/* ---- SDL_image 解码 ----------------------------------------------------
 *
 * 【2026-09 换库】原先直接链系统 libpng16 / libjpeg：
 *   - 库名是 Unix 专有（Windows 上既没有 "png16" 也没有 "jpeg" 这个库名），
 *     且要求使用者先装 libpng-dev / libjpeg-dev；
 *   - libjpeg 还得自己写 setjmp 错误回调 + 手工翻 RGB→RGBA 行。
 * 现在统一走 SDL2_image：它自带 libpng/libjpeg/zlib（external/ 目录），
 * 由 xmake 统一拉取编译，各平台一致；解码也只管"拿一张 RGBA 出来"。
 *
 * 注意两处刻意保留的东西：
 *   1) png_meta()：自己扫块读 IHDR 的 color type 与 tRNS 透明色。
 *      SDL_image 只给表面（surface），不给"固件语义的 format/透明色"，
 *      而这两个值要填进 IImage 对象（见 g_last_fmt/g_last_tc）。
 *   2) 输出**字节序固定为 R,G,B,A**（原 png 用 PNG_FORMAT_RGBA、jpeg 手工展开
 *      也是这个序），下游 fb_blit_rgba 等按这个序解释，不能换。
 * --------------------------------------------------------------------- */

/* SDL_image 惰性初始化（PNG + JPG 两个解码器都要就位） */
static void zm_img_ready(void) {
  static int inited = 0;
  if (inited)
    return;
  inited = 1;
  const int want = IMG_INIT_PNG | IMG_INIT_JPG;
  int got = IMG_Init(want);
  if ((got & want) != want)
    log_warn("SDL_image 初始化不全（得到 0x%X / 需要 0x%X）: %s —— "
             "PNG/JPG 解码可能失败", got, want, IMG_GetError());
}

/* 前 4 字节是否是 PNG 魔数（\x89PNG） */
static int is_png_magic(const uint8_t *d, size_t len) {
  return len >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
}
/* 是否是 JPEG 魔数（FF D8 FF） */
static int is_jpg_magic(const uint8_t *d, size_t len) {
  return len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
}

/* 用 SDL_image 把内存里的图片解成 R,G,B,A（每像素 4 字节）。
 * 成功 0 并填 *ow/*oh/*orgba（malloc 得到，调用方 free）；失败 -1。 */
static int decode_via_sdl(const uint8_t *data, size_t len, int *ow, int *oh,
                          uint8_t **orgba) {
  zm_img_ready();
  SDL_RWops *rw = SDL_RWFromConstMem(data, (int)len);
  if (!rw) {
    log_warn("SDL_RWFromConstMem 失败: %s", SDL_GetError());
    return -1;
  }
  SDL_Surface *s = IMG_Load_RW(rw, 1); /* 1 = 顺手释放 rw */
  if (!s) {
    log_warn("SDL_image 解码失败: %s", IMG_GetError());
    return -1;
  }
  /* 【踩过的坑】要的是**内存字节序 R,G,B,A**，所以必须用 SDL_PIXELFORMAT_RGBA32，
   * 不能用 SDL_PIXELFORMAT_RGBA8888 —— SDL 的 *_8888 名字描述的是**32 位值的
   * 位序（MSB→LSB）**，不是内存字节序：RGBA8888 在小端机上内存里是 A,B,G,R。
   * 实测（00000506 的 index_bg.jpg，原图 (0,0)=(33,89,136)）：
   *   用 RGBA8888 → 解出 4 字节 = 255,136,89,33（= A,B,G,R），整屏发出紫色调；
   *   RGBA32（小端下 = ABGR8888）→ 33,89,136,255 ✓。
   * RGBA32/ARGB32 这组宏由 SDL 按端序自动选，大端机上也对，别写死 8888。
   * 完整的"哪条边界该用哪个格式"见 docs/图像与像素格式.md。 */
  SDL_Surface *conv = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(s);
  if (!conv) {
    log_warn("SDL_ConvertSurfaceFormat(RGBA32) 失败: %s", SDL_GetError());
    return -1;
  }
  int w = conv->w, h = conv->h;
  uint8_t *rgba = malloc((size_t)w * (size_t)h * 4u);
  if (!rgba) {
    SDL_FreeSurface(conv);
    return -1;
  }
  if (SDL_MUSTLOCK(conv))
    SDL_LockSurface(conv);
  for (int y = 0; y < h; y++)
    memcpy(rgba + (size_t)y * (size_t)w * 4u,
           (const uint8_t *)conv->pixels + (size_t)y * (size_t)conv->pitch,
           (size_t)w * 4u);
  if (SDL_MUSTLOCK(conv))
    SDL_UnlockSurface(conv);
  SDL_FreeSurface(conv);
  *ow = w;
  *oh = h;
  *orgba = rgba;
  return 0;
}

static int decode_png(const uint8_t *data, size_t len, int *ow, int *oh,
                      uint8_t **orgba) {
  /* 先取固件语义的 format / 透明色（SDL_image 不提供） */
  {
    int mf = 2;
    uint32_t mtc = 0xFFFFFFFFu;
    png_meta(data, len, &mf, &mtc);
    g_last_fmt = mf;
    g_last_tc = mtc;
  }
  /* decode_any 的兜底分支会拿"非 PNG 数据"来试这个函数，魔数不符直接拒掉，
   * 否则 SDL_image 会按内容自动识别、把 JPEG 也当 PNG 解成功，
   * decode_any 报出的 *type 就错了。 */
  if (!is_png_magic(data, len)) {
    log_warn("PNG 解析失败: 魔数不符");
    return -1;
  }
  return decode_via_sdl(data, len, ow, oh, orgba);
}

static int decode_jpg(const uint8_t *data, size_t len, int *ow, int *oh,
                      uint8_t **orgba) {
  g_last_fmt = 2;          /* RE：JPG 走 format 2 */
  g_last_tc = 0xFFFFFFFFu; /* 不透明 */
  if (!is_jpg_magic(data, len)) {
    log_warn("JPEG 解析失败: 魔数不符");
    return -1;
  }
  return decode_via_sdl(data, len, ow, oh, orgba);
}

/* 按内容魔数选择解码器（applet 传的文件名可能没有扩展名）。
 * 同时报出实际命中的格式 → *type（真机 IImage 的类型字段）。 */
static int decode_any(const uint8_t *data, size_t len, int *w, int *h,
                      uint8_t **rgba, int *type) {
  if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
      data[3] == 'G') {
    *type = ZM_IMG_TYPE_PNG;
    return decode_png(data, len, w, h, rgba);
  }
  if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
    *type = ZM_IMG_TYPE_JPG;
    return decode_jpg(data, len, w, h, rgba);
  }
  /* GIF（含 GIF87a/GIF89a）与 BMP：直接交给 SDL2_image。
   *
   * RE：00000442《驱蚊大师》把**内嵌在自身 payload 里的 GIF**
   * （文件偏移 102012 处即 "GIF89a"）直接塞进 IImage::SetData，
   * 没有文件 I/O；固件的类型枚举只有 0=GIF/1=PNG/2=JPG，所以
   * GIF 用 0（原样保真），BMP 也归到 0（非 PNG/JPG 位图）。
   * 动图只解第一帧（GetFrameCount 返回 1），applet 未据帧号分支。 */
  if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' &&
      data[3] == '8') {
    *type = ZM_IMG_TYPE_GIF;
    return decode_via_sdl(data, len, w, h, rgba);
  }
  if (len >= 2 && data[0] == 'B' && data[1] == 'M') {
    *type = ZM_IMG_TYPE_GIF;
    return decode_via_sdl(data, len, w, h, rgba);
  }
  /* 兜底：先按 PNG，再按 JPEG 试一遍（某些文件头带偏移） */
  if (decode_png(data, len, w, h, rgba) == 0) {
    *type = ZM_IMG_TYPE_PNG;
    return 0;
  }
  *type = ZM_IMG_TYPE_JPG;
  return decode_jpg(data, len, w, h, rgba);
}

/* 读文件 + 解码，填入记录；成功返回 0 */
static int rec_load_file(zm_img_rec *r, const char *name) {
  uint8_t *raw = NULL;
  size_t raw_len = 0;
  if (zm_fs_read_file(name, &raw, &raw_len) != 0)
    return -1;

  int w = 0, h = 0, type = ZM_IMG_TYPE_GIF;
  uint8_t *rgba = NULL;
  int rc = decode_any(raw, raw_len, &w, &h, &rgba, &type);
  free(raw);
  if (rc != 0) {
    log_warn("IImage: 无法解码 \"%s\"（%zu 字节）", name, raw_len);
    return -1;
  }
  rec_free_pixels(r);
  r->w = w;
  r->h = h;
  r->type = type;
  r->rgba = rgba;
  snprintf(r->name, sizeof(r->name), "%s", name);
  log_info("IImage: 载入 \"%s\" %dx%d type=%d(%s)", name, w, h, type,
           type == ZM_IMG_TYPE_PNG ? "PNG"
                                   : (type == ZM_IMG_TYPE_JPG ? "JPG" : "GIF"));
  return 0;
}

/* 这段字节像不像"图像数据本身"（而不是文件名字符串）。
 * 用于区分 SetData 的两种调用形态，见 zm_image_SetData 的长注释。 */
static bool looks_like_image_magic(const uint8_t *m) {
  if (m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G')
    return true; /* PNG */
  if (m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF)
    return true; /* JPEG */
  if (m[0] == 'G' && m[1] == 'I' && m[2] == 'F' && m[3] == '8')
    return true; /* GIF87a / GIF89a */
  if (m[0] == 'B' && m[1] == 'M')
    return true;                     /* BMP */
  if (m[0] == 'R' && m[1] == 'I' && m[2] == 'F' && m[3] == 'F')
    return true; /* RIFF（WEBP） */
  return false;
}

/* 从**客户机内存**里的图像数据解码，填入记录；成功返回 0。
 * 对应 00000442 那种"数据在 payload 里、直接给指针"的形态。 */
static int rec_load_mem(uc_engine *uc, zm_img_rec *r, uint32_t ptr,
                        uint32_t len) {
  if (!len || len > 64u * 1024u * 1024u) /* 防御：别被脏长度骗着读一大片 */
    return -1;
  uint8_t *raw = malloc(len);
  if (!raw)
    return -1;
  if (uc_mem_read(uc, ptr, raw, len) != UC_ERR_OK) {
    free(raw);
    return -1;
  }
  int w = 0, h = 0, type = ZM_IMG_TYPE_GIF;
  uint8_t *rgba = NULL;
  int rc = decode_any(raw, len, &w, &h, &rgba, &type);
  free(raw);
  if (rc != 0) {
    log_warn("IImage: 无法解码内存图像 0x%X（%u 字节）", ptr, len);
    return -1;
  }
  rec_free_pixels(r);
  r->w = w;
  r->h = h;
  r->type = type;
  r->rgba = rgba;
  snprintf(r->name, sizeof(r->name), "<mem 0x%X>", ptr);
  log_info("IImage: 载入内存图像 0x%X %u 字节 -> %dx%d type=%d(%s)", ptr, len, w,
           h, type,
           type == ZM_IMG_TYPE_PNG ? "PNG"
                                   : (type == ZM_IMG_TYPE_JPG ? "JPG" : "GIF"));
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
  uc_write32(uc, obj, IMAGE_VT_ADDR);
  uc_write32(uc, obj + 4, 0);
  uc_write32(uc, obj + 8, surf);
  /* +0x18 = 类型字段（IImage::GetType 读的就是这里）。
   * 真机由解码器按文件魔数写入（PNG=1）；我们尚无"魔数→枚举"的完整
   * 证据，显式写 0（未识别）—— 比依赖"池初始为 0"更明确，也避免槽复用
   * 时残留上一张图的类型。 */
  uc_write32(uc, obj + IMAGE_ENTRY_OFF_TYPE, 0);

  /* surface：首字=自己的虚表（对象池里仍按 surf 识别）；其余为解码元数据 */
  uc_write32(uc, surf, IMAGE_VT_ADDR);

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

/* +0x08 SetData(this, mode, ptr, len)
 *
 * 真机的语义：把 (ptr, len) 存进对象（+0xC=原始数据指针、+0x10=长度）并按
 * **魔数**定下类型字段 +0x18。也就是说这个 ptr **既可能是文件名、也可能
 * 是图像数据本身**，取决于 applet 怎么用——真机的这种不确定性正好解释了
 * 为什么它要"按魔数定类型"。
 *
 * 实测两种形态都存在：
 *   (a) 文件名：`sprintf("res\\xxx.png")` 拼出的串，len = strlen
 *       —— 计算器/捕鱼等多数 applet。此时魔数不是图像 → 按路径去读盘。
 *   (b) 内存数据：00000442《驱蚊大师》把**内嵌在自身 payload 里的 GIF**
 *       （文件偏移 102012 处是 "GIF89a"）直接塞进来，len = 数据字节数。
 *       它一秒钟能重试 100 多次：日志里刷 "zm_fs_read_file: 找不到文件
 *       "GIF89a"" 152 次、**一张图都没加载成功**，结果是游戏画面里
 *       只剩文字（计时器数字都是 bitmap 挂不上），倒计时永远停在 00:01:00。
 *
 * 判据：ptr 的头几个字节是不是图像魔数（PNG/JPEG/GIF/BMP/RIFF）。
 * 文件名的可打印 ASCII 前缀不可能撞上这些魔数（都含 >=0x80 的字节，
 * 或 "GIF8"/"BM" 这种在路径里不可能出现的开头）。
 *
 * 返回 0 = 成功；非 0 = 失败（applet 会立刻 Release）。 */
uint32_t zm_image_SetData(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                          uint32_t r3) {
  (void)r1; /* mode/flag，实测恒为 0 */
  /* 像素挂在 entry+8 的 surface 上，entry 自身不持像素 */
  zm_img_rec *r = pixel_rec(r0);
  if (!r)
    return (uint32_t)-1;

  uint8_t magic[6] = {0};
  if (r2 && r3 && uc_mem_read(uc, r2, magic, sizeof(magic)) == UC_ERR_OK &&
      looks_like_image_magic(magic)) {
    if (rec_load_mem(uc, r, r2, r3) != 0)
      return (uint32_t)-1;
    uc_write32(uc, r0 + IMAGE_ENTRY_OFF_TYPE, (uint32_t)r->type);
    return 0;
  }

  char name[256];
  read_cstr(uc, r2, name, sizeof(name));
  if (name[0] == '\0' && r3)
    return (uint32_t)-1;
  if (rec_load_file(r, name) != 0)
    return (uint32_t)-1;
  /* 类型写回**传入对象**的 +0x18（GetType / 真机 Decode 都读这里）。
   * 传进来的一定是 entry（CreateImage 的产物），不是 surf。 */
  uc_write32(uc, r0 + IMAGE_ENTRY_OFF_TYPE, (uint32_t)r->type);
  return 0;
}

/* +0x0C GetFrameCount(this) → 帧数（静态图返回 1；applet 未据此分支） */
uint32_t zm_image_GetFrameCount(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 1;
}

/* +0x10 Width(this) → 宽（像素） */
uint32_t zm_image_Width(uc_engine *uc, uint32_t r0) {
  (void)uc;
  zm_img_rec *r = pixel_rec(r0);
  return (uint32_t)(r ? r->w : 0);
}

/* +0x14 Height(this) → 高（像素） */
uint32_t zm_image_Height(uc_engine *uc, uint32_t r0) {
  (void)uc;
  zm_img_rec *r = pixel_rec(r0);
  return (uint32_t)(r ? r->h : 0);
}

/* +0x18 GetType(this) → 对象 +0x18 的**类型字段**
 *
 * 真机（ZMAEE_IImage_GetType @0x30490）没有逻辑，就是一个字段 getter：
 *     CMP  R0, #0
 *     BEQ  loc_30498          ; 空对象 → 返回 -4
 *     LDR  R0, [R0,#0x18]     ; 否则原样读出 +0x18
 *     BX   LR
 * loc_30498: MOVS R0,#4 / NEGS R0,R0   ; R0 = -4
 *
 * 类型值由 SetData/装入器按文件魔数写入（见 ZM_IMG_TYPE_*：0=GIF 1=PNG 2=JPG，
 * 也是 ZMAEE_IImage_Decode 的分派键；固件 ZMAEE_IDisplay_DrawImage 同样用
 * `GetType()==1` 判 PNG 专用路径）。
 * 【2026-09 修正】旧实现恒返回 0：一是空对象时应返回 -4（applet 若用
 * `< 0` 判失败会走错分支），二是丢掉了"读字段"这一语义，三是 0 在真机
 * 枚举里恰恰是 **GIF**。现按真机照读，字段由 SetData 按魔数回填。 */
uint32_t zm_image_GetType(uc_engine *uc, uint32_t r0) {
  if (!r0)
    return (uint32_t)-4; /* 真机：空对象哨兵 */

  uint32_t t = uc_read32(uc, r0 + IMAGE_ENTRY_OFF_TYPE);
  /* 临时探针（RE 用，确认后删）：applet 到底有没有调这个槽？调的时候
   * 对象是哪张图？—— 决定要不要把魔数映射成真机的类型枚举。 */
  {
    static int n = 0;
    if (n < 8) {
      uint32_t lr = 0;
      uc_reg_read(g_uc, UC_ARM_REG_LR, &lr);
      zm_img_rec *r = pixel_rec(r0);
      n++;
      log_info("[IImage.GetType] obj=0x%X → %u (%s) 调用点 0x%X", r0, t,
               r && r->name[0] ? r->name : "池外对象", lr);
    }
  }
  return t;
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
  uc_write32(uc, surf_obj, SURF_VT_ADDR);

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
    /* RE：format/透明色由解码器决定（见 png_meta）：
     *   调色板 → 0、truecolor → 2、RGBA → 3；tRNS 有则取之，否则 -1 */
    uc_write32(uc, surf_obj + 16, (uint32_t)g_last_fmt);
    uc_write32(uc, surf_obj + 20, g_last_tc);
    uc_write32(uc, surf_obj + 24, 0);           /* 无调色板 */
    uc_write32(uc, surf_obj + 28, 0);
    uc_write32(uc, surf_obj + 32, 0);
    uc_write32(uc, surf_obj + 36, gpx); /* 像素指针 */
    uc_write32(uc, surf_obj + 40, 0);
    log_info("IImage::Decode -> IBitmap@0x%X %dx%d fmt=%d trans=0x%08X pix@0x%X",
             surf_obj, src->w, src->h, g_last_fmt, g_last_tc, gpx);
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
     * 堆对象）。旧实现把 out[0] 写成虚表地址 SURF_VT_ADDR，
     * 于是每次绘制都拿到 0x7A3200（虚表本身）→ 找不到像素 → 733 次失败。
     * 这里写入 surf_obj（对象首字即 SURF_VT_ADDR，可正常响应 vt 调用）。 */
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

/* SURF_VT_ADDR+0x10 = **IBitmap::GetInfo(this, out)**
 *
 * 真机虚表依据：libaee.so 的 `g_aee_bitmap_vtbl` @ 0x63DF4：
 *   +0x00 AddRef  +0x04 Release  +0x08 SetTransColor  +0x0C ?
 *   +0x10 **ZMAEE_IBitmap_GetInfo**  +0x14 ?  +0x18 ?
 * 而 `ZMAEE_IBitmap_GetInfo` 的实现就是 `memcpy(out, bitmap + 8, 32)`，
 * 于是 out 的 8 个 dword 恰好是 IBitmap 字段：
 *   +0 宽  +4 高  +8 颜色格式  +12 透明色
 *   +16 调色板标志  +20 调色板指针  +24 调色板大小  +28 像素指针
 *
 * applet 用法（00000506 sub_388 type1）：
 *   (*(a1[3]->vt + 0x10))(a1[3], v10);      // 填 v10
 *   v13 = v10[0]; v14 = v10[1];             // ← 取**宽高**（各一个 32 位字）
 *   DrawBitmap(disp, x, y, a1[3], {0,0,v13,v14}, ...)
 *
 * 【2026-09 修正】旧实现按 int16 写了 4 个短整型（且顺序是 rect 的 l,t,r,b），
 * applet 读到的是 v10[0]=(t<<16)|l、v10[1]=(h<<16)|w ——
 * 实测 31x39 的对象在 applet 侧变成 rect={0,0,0,2555935}，
 * 而 2555935 = 39*65536+31 ✓ 完全吻合；这个"尺寸"又被 applet 用于居中
 * 计算，于是出现 y≈−h×32768 的诡异坐标（关卡选择界面 9 张图全部画到屏外）。
 * 现在按真机语义：直接把客户机对象 +8 起的 32 字节原样拷进 out。 */
uint32_t zm_surf_getrect(uc_engine *uc, uint32_t r0, uint32_t r1) {
  if (r1) {
    uint32_t info[8] = {0};
    if (uc_mem_read(uc, r0 + 8, info, sizeof(info)) != UC_ERR_OK) {
      zm_img_rec *r = pixel_rec(r0);
      info[0] = (uint32_t)(r ? r->w : 0);
      info[1] = (uint32_t)(r ? r->h : 0);
    }
    uc_mem_write(uc, r1, info, sizeof(info));
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
 *   fmt 3 = 32bpp    （4 字节；copy32，**0xAARRGGBB**，靠 alpha 定透明）
 *   fmt 4 = P32      （4 字节，**0xAARRGGBB**；copyP32，同样靠 alpha 定透明）
 *
 * 实测验证：背景 surface {240,320,1,0} 的数据是 0x22D1/0x22D0（RGB565 深蓝灰），
 * 精灵 surface {49,26,3,0xF81F} 的数据是 0x00292828（32bpp 深灰）——
 * 与上述映射完全吻合。**注意 fmt 1 不是 8bpp 索引色**，早期按索引色处理
 * 会把 RGB565 的高低字节拆成两个像素，画面呈细密噪点。
 *
 * 32bpp（fmt 3/4）的字节序：文件里是 B,G,R,A，小端读成 dword 正好是
 * 0xAARRGGBB —— 与固件 Copy32To16 的通道拆分（R=v[23:19]）完全一致，
 * 所以**不需要交换 R/B**（曾怀疑过，已用 number1.zmspx tex0 验证：
 *  bytes 00 F6 FF FF → 固件算出 565 = 0xFFA0 = 金黄，正确）。
 *
 * surf[0xC] 在这里按"透明色"解释（精灵实测 0xF81F = RGB565 洋红）：
 * 比较时按当前格式截断 —— 对 32bpp 源这个比较实际不命中（源靠 alpha），
 * 保留只为兼容 fmt 1/2 的 Mask 语义。
 *
 * 返回 0 表示透明（跳过），-1 读失败。
 * 注意：32bpp 的返回值**保留 alpha**（不再抹成 0xFF），混合由调用方处理。 */
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
  /* fmt 3 = 32bpp；fmt 4 = P32 —— 两者都是 0xAARRGGBB（alpha 在**最高字节**）
   *
   * RE：固件 ZMAEE_Copy32To16 / ZMAEE_CopyP32To16
   *     （安卓 libaee.so.c:48334 / :48472，与 `.text` 版逐条同构）：
   *
   *       a5 = v >> 27;                     ← alpha 就是**高 5 位**
   *       a5 == 31 → 直接写 RGB565（不透明）
   *       a5 == 0  → 整像素不写（透明）
   *       1..30    → 与目标像素做 565 空间混合：dst + ((src - dst) * a5) >> 5
   *       通道拆分：R = v[23:19]，G = v[15:10]，B = v[7:3]
   *
   *     两个函数都**不改 RGB 通道顺序**（源就是 ARGB8888），只有 alpha 语义。
   *     applet 自带的 sub_61B4 / sub_6248 是这两个函数的逐字内联副本。
   *
   * 【2026-09 修正】旧实现写下 `0xFF000000 | (v & 0xFFFFFF)`，等于把 alpha
   * 抹成不透明。实测本 applet 全部 type=3 纹理里 alpha=0 的像素占 31.2%
   * （32007/102476），它们被整片画成不透明 → 画面上出现白/灰色方块
   * （例：index_menu.zmspx tex6 49x26，403/1274 像素 alpha=0，RGB 为
   *  282929/ffffff 的"废色"，只有靠 alpha 才能判透明）。
   * 现在保留 alpha 原样返回，a5 的判定与混合交给调用方（它需要目的像素）。 */
  uint32_t v = 0;
  if (uc_mem_read(uc, addr, &v, 4) != UC_ERR_OK)
    return -1;
  if ((v & 0xFFFFFFu) == (ck & 0xFFFFFFu))
    return 0;
  if ((v >> 27) == 0)
    return 0; /* a5 == 0 → 全透明（固件语义：整像素不写） */
  *out_argb = (int)v;
  return 1;
}

/* 固件 ZMAEE_Copy32To16 的混合（在 0..255 尺度上做等价换算）：
 * dst + ((src - dst) * a5) / 31，a5 ∈ [1,30]。 */
static inline unsigned gdi_blend5(unsigned d, unsigned s, unsigned a5) {
  int delta = (int)s - (int)d;
  int step = (delta * (int)a5 + (delta < 0 ? -15 : 15)) / 31;
  int r = (int)d + step;
  return (unsigned)(r < 0 ? 0 : (r > 255 ? 255 : r));
}

int zm_image_blit_gdi_surface(uc_engine *uc, uint32_t surf, int dx, int dy,
                              uint32_t rect_ptr, int mode) {
  int w = 0, h = 0, step = 0;
  uint32_t ck = 0;
  if (!zm_image_get_gdi_surface(uc, surf, &w, &h, &step, &ck))
    return 0;

  int sx = 0, sy = 0, sw = w, sh = h;
  if (rect_ptr) {
    /* rect = {x, y, w, h}（与 UpdateEx / DrawText 一致，见
     * zm_display.c blit_surface_region 的说明） */
    int rx = (int)uc_read32(uc, rect_ptr);
    int ry = (int)uc_read32(uc, rect_ptr + 4);
    int rw = (int)uc_read32(uc, rect_ptr + 8);
    int rh = (int)uc_read32(uc, rect_ptr + 12);
    if (rw > 0 && rh > 0) {
      sx = rx;
      sy = ry;
      sw = rw;
      sh = rh;
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

  /* 首次遇到某个 (step, ck) 组合就打一行 —— 用最低的噪音记录
   * "这个 applet 到底用了哪些像素格式"，诊断 fmt/alpha 语义时的关键依据
   * （例如判断有没有精灵真的走 2 字节/LA88 路径）。 */
  {
    static uint32_t seen[24];
    static int nseen = 0;
    uint32_t key = step * 1000003u + (ck & 0xFFFFFFu) + (uint32_t)(mode & 7) * 7919u;
    int known = 0;
    for (int i = 0; i < nseen; i++)
      if (seen[i] == key) {
        known = 1;
        break;
      }
    if (!known) {
      if (nseen < 24)
        seen[nseen++] = key;
      log_info("[GDI 首次] step=%d(每像素%d字节) ck=0x%X mode=%d 尺寸=%dx%d "
               "目标=(%d,%d) surf=0x%X pix@0x%X",
               step, bytes, ck, mode & 7, w, h, dx, dy, surf, base);
    }
  }

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
      /* mode（固件 ZMAEE_GDI_BitBlt_Ext 的 a6）→ 变换。
       * 完整 RE 依据见 zm_display.c 的 fb_blit_rgba_mode 注释（同一张表）。
       *   0 恒等 (X,Y)     1 左右翻转 (-X,Y)        2 反对角镜像 (-Y,-X)
       *   3 上下翻转 (X,-Y) 4 主对角镜像/转置 (Y,X)  5 90° (-Y,X)
       *   6 180° (-X,-Y)    7 270° (Y,-X)
       * 【2026-09 修正】旧的 1/2/3/6/7 是错位表：mode7 应为 270° 旋转
       * （net.zmspx 35x35 渔网走 mode5/7），旧实现做成反对角镜像 →
       * 方形精灵看起来是象限转置。 */
      switch (mode & 7) {
      case 1: ddx = sw - 1 - x; break;
      case 2: ddx = sh - 1 - y; ddy = sw - 1 - x; break;
      case 3: ddy = sh - 1 - y; break;
      case 4: ddx = y; ddy = x; break;
      case 5: ddx = sh - 1 - y; ddy = x; break;
      case 6: ddx = sw - 1 - x; ddy = sh - 1 - y; break;
      case 7: ddx = y; ddy = sw - 1 - x; break;
      default: break;
      }
      int ox = dx + ddx, oy = dy + ddy;
      /* 32bpp 源的 alpha 语义（见 gdi_read_pixel 的 RE 说明）：
       *   高 5 位 = alpha；0 已被 gdi_read_pixel 挡掉，
       *   31 = 不透明（直接写），1..30 → 与目标像素按比例混合后写。
       * 混合基准取宿主帧缓冲（与 fb_blit_rgba 的 PNG 路径同一套做法）；
       * 固件是在 RGB565 空间对**层**混合，换算到 888 后差异 ≤1 个 5bit 步进。
       * fmt 1/2 的返回像素 alpha 恒为 0xFF → a5 = 31，行为与以前完全一致。 */
      unsigned a5 = ((unsigned)argb >> 27) & 0x1Fu;
      uint32_t out = (uint32_t)argb & 0x00FFFFFFu;
      if (a5 < 31u) {
        int fw = 0, fh = 0;
        uint32_t *fb = zm_fb_buffer(&fw, &fh);
        if (fb && (unsigned)ox < (unsigned)fw && (unsigned)oy < (unsigned)fh) {
          uint32_t d = fb[(size_t)oy * (size_t)fw + (size_t)ox];
          unsigned r = gdi_blend5((d >> 16) & 0xFFu, (out >> 16) & 0xFFu, a5);
          unsigned g = gdi_blend5((d >> 8) & 0xFFu, (out >> 8) & 0xFFu, a5);
          unsigned b = gdi_blend5(d & 0xFFu, out & 0xFFu, a5);
          out = (r << 16) | (g << 8) | b;
        }
      }
      zm_fb_write(ox, oy, 0xFF000000u | out);
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

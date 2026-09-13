#include "zm_layer.h"

#include <string.h>

#include "../../emu.h"
#include "../../tool/uc_helper.h" /* uc_read32 / uc_write32 */
#include "../../log/log.h"
#include "zm_image.h" /* zm_pix_pool_alloc：层缓冲也从像素池分配 */
#include "zm_display.h" /* zm_display_GetBaseLayerBuffer：层 0 = 基础层 */

/* IDisplay 层数组距对象起点的偏移（RE：层项基址 = IDisplay + 52*idx，
 * 载荷 = 基址 + 36） */
#define LAYER_PAYLOAD_OFF 36u
#define LAYER_STRIDE 52u

uint32_t zm_layer_payload(uint32_t display, uint32_t idx) {
  return display + LAYER_PAYLOAD_OFF + LAYER_STRIDE * idx;
}

static void zero_guest(uc_engine *uc, uint32_t addr, uint32_t bytes) {
  static uint8_t zb[4096];
  for (uint32_t off = 0; off < bytes; off += sizeof(zb)) {
    uint32_t n = bytes - off;
    if (n > sizeof(zb))
      n = sizeof(zb);
    uc_mem_write(uc, addr + off, zb, n);
  }
}

int zm_layer_get(uc_engine *uc, uint32_t display, uint32_t idx, zm_layer_t *out) {
  if (!out)
    return -1;
  memset(out, 0, sizeof(*out));
  if (display == 0 || idx > 0xF)
    return -1;
  uint32_t P = zm_layer_payload(display, idx);
  uint32_t head[5]; /* +0x00 色深 +0x04 x +0x08 y +0x0C 宽 +0x10 高 */
  if (uc_mem_read(uc, P, head, sizeof(head)) != UC_ERR_OK)
    return -1;
  out->fmt = head[0];
  out->x = head[1];
  out->y = head[2];
  out->w = head[3];
  out->h = head[4];
  out->cx = uc_read32(uc, P + 0x14);
  out->cy = uc_read32(uc, P + 0x18);
  out->cw = uc_read32(uc, P + 0x1C);
  out->ch = uc_read32(uc, P + 0x20);
  out->buf = uc_read32(uc, P + 0x24);
  out->tenable = uc_read32(uc, P + 0x2C);
  out->tcolor = uc_read32(uc, P + 0x30);
  return out->buf ? 0 : -1;
}

uint32_t zm_layer_CreateLayer(uc_engine *uc, uint32_t display, uint32_t idx,
                              uint32_t rect_ptr, uint32_t fmt) {
  /* RE：ZMAEE_IDisplay_CreateLayer
   *   if (display == 0 || (idx-1) > 0xE || rect == 0 || (fmt-1) > 3) return -4;
   *   v8 = display + 52*idx;  if (v8[18] != 0) return -8;    // 已存在
   *   分配 = bpp * w * h（bpp: fmt==1 → 2，其余 → 4），malloc 后清零
   *   标志字节 display[idx + 20] = 16                        // 缓冲自持
   *   memset(项载荷, 0, 52)
   *   v8[9] = fmt   v8[10] = x   v8[11] = y
   *   v8[12] = w    v8[13] = h   v8[14] = 0  v8[15] = 0
   *   v8[16] = w    v8[17] = h   v8[18] = buf
   * 注意 v8 是 (display + 52*idx)，故 v8[9] 就是载荷 +0x00。 */
  if (display == 0 || idx < 1 || idx > 15 || rect_ptr == 0)
    return (uint32_t)-4;
  if (fmt < 1 || fmt > 4)
    return (uint32_t)-4;

  uint32_t P = zm_layer_payload(display, idx);
  if (uc_read32(uc, P + 0x24) != 0)
    return (uint32_t)-8; /* 该层已存在 */

  uint32_t rect[4];
  if (uc_mem_read(uc, rect_ptr, rect, sizeof(rect)) != UC_ERR_OK)
    return (uint32_t)-4;
  int w = (int)rect[2];
  int h = (int)rect[3];
  if (w <= 0 || h <= 0)
    return (uint32_t)-4;

  uint32_t bpp = (fmt == 1) ? 2u : 4u;
  uint32_t bytes = (uint32_t)w * (uint32_t)h * bpp;
  uint32_t buf = zm_pix_pool_alloc(bytes);
  if (!buf)
    return (uint32_t)-2;
  zero_guest(uc, buf, bytes);

  uint8_t own = 16; /* RE：*(BYTE*)(display + idx + 20) = 16 */
  uc_mem_write(uc, display + idx + 20, &own, 1);

  uint8_t pl[LAYER_STRIDE];
  memset(pl, 0, sizeof(pl));
  memcpy(pl + 0x00, &fmt, 4);
  memcpy(pl + 0x04, &rect[0], 4);
  memcpy(pl + 0x08, &rect[1], 4);
  memcpy(pl + 0x0C, &rect[2], 4);
  memcpy(pl + 0x10, &rect[3], 4);
  memcpy(pl + 0x1C, &rect[2], 4); /* 宽副本 */
  memcpy(pl + 0x20, &rect[3], 4); /* 高副本 */
  memcpy(pl + 0x24, &buf, 4);
  if (uc_mem_write(uc, P, pl, sizeof(pl)) != UC_ERR_OK)
    return (uint32_t)-1;

  log_info("CreateLayer(层=%u %dx%d fmt=%u) 缓冲@0x%X", idx, w, h, fmt, buf);
  return 0;
}

uint32_t zm_layer_GetLayerInfo(uc_engine *uc, uint32_t display, uint32_t idx,
                               uint32_t out_ptr) {
  /* RE：ZMAEE_IDisplay_GetLayerInfo
   *   display==0 / idx>0xF / out==0 → -4；项 +72（即载荷 +0x24）为 0 → -4
   *   否则 memcpy(out, 载荷, 0x34) */
  if (display == 0 || idx > 0xF || out_ptr == 0)
    return (uint32_t)-4;
  uint32_t P = zm_layer_payload(display, idx);

  /* 层 0 = 基础层（RE：FreeAllLayer 从 i=1 起、从不释放层 0）。
   * 它应常驻指向基础层缓冲；若首次查询时尚未建立，在此惰性填好，
   * 否则 applet 会把 GetLayerInfo(0) 失败当作致命错误而 abort。
   * 仅当 idx==0 时才触发，层 1..15 仍由 CreateLayer 显式建立，互不影响。 */
  if (idx == 0 && uc_read32(uc, P + 0x24) == 0) {
    uint32_t base = zm_display_GetBaseLayerBuffer(uc);
    if (base) {
      uint32_t fmt = 1, x0 = 0, y0 = 0, w = LAYER_W, h = LAYER_H;
      uint8_t pl[LAYER_STRIDE];
      memset(pl, 0, sizeof(pl));
      memcpy(pl + 0x00, &fmt, 4);
      memcpy(pl + 0x04, &x0, 4);
      memcpy(pl + 0x08, &y0, 4);
      memcpy(pl + 0x0C, &w, 4);
      memcpy(pl + 0x10, &h, 4);
      memcpy(pl + 0x1C, &w, 4);
      memcpy(pl + 0x20, &h, 4);
      memcpy(pl + 0x24, &base, 4);
      uc_mem_write(uc, P, pl, sizeof(pl));
      log_info("GetLayerInfo(层=0) 惰性建立基础层 buf=0x%X (%dx%d RGB565)", base,
               w, h);
    }
  }

  if (uc_read32(uc, P + 0x24) == 0) {
    static uint32_t nf = 0;
    if (nf++ < 4)
      log_info("GetLayerInfo(层=%u) 失败：层无缓冲", idx);
    return (uint32_t)-4;
  }
  uint8_t pl[LAYER_STRIDE];
  if (uc_mem_read(uc, P, pl, sizeof(pl)) != UC_ERR_OK)
    return (uint32_t)-4;
  uc_mem_write(uc, out_ptr, pl, sizeof(pl));
  {
    static uint32_t n = 0;
    if (n++ < 6)
      log_info("GetLayerInfo(层=%u) OK buf=0x%X", idx,
               uc_read32(uc, P + 0x24));
  }
  return 0;
}

uint32_t zm_layer_SetActiveLayer(uc_engine *uc, uint32_t display, uint32_t idx) {
  /* RE：display==0 / idx>0xF / 项 +72 == 0 → -4；否则 *(display+8) = idx */
  if (display == 0 || idx > 0xF)
    return (uint32_t)-4;
  if (uc_read32(uc, zm_layer_payload(display, idx) + 0x24) == 0)
    return (uint32_t)-4;
  uc_write32(uc, display + 8, idx);
  return 0;
}

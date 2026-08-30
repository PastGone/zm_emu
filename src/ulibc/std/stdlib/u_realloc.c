#include "../../include/u_heap.h"

#include "../../include/internal/u_heap_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_realloc.c
 * @brief u_realloc —— 重分配：优先原地扩张，否则搬迁
 *
 * 注意：缩小会**合法地**丢弃超出新长度的数据（标准 realloc 语义）。
 */

uint32_t u_realloc(uc_engine *uc, uint32_t p, uint32_t size) {
  if (!u_heap_ready(uc))
    return 0;
  if (p == 0)
    return u_malloc(uc, size);
  if (size == 0) {
    u_free(uc, p);
    return 0;
  }
  if (p < u_heap_g.base + U_HEAP_HDR || p >= u_heap_g.end)
    return 0;

  uint32_t hdr = p - U_HEAP_HDR;
  uint32_t old_total = u_blk_total(uc, hdr);
  if (old_total < U_BLK_MIN || hdr + old_total > u_heap_g.end)
    return 0;
  uint32_t old_payload = old_total - U_HEAP_HDR;

  uint32_t need = u_heap_align8(size);
  if (need < U_HEAP_ALIGN)
    need = U_HEAP_ALIGN;

  if (need <= old_payload) {
    /* 缩小：若剩余足够成块则分裂，避免浪费 */
    uint32_t new_total = need + U_HEAP_HDR;
    uint32_t rest = old_total - new_total;
    if (rest >= U_BLK_MIN) {
      u_blk_set_size(uc, hdr, new_total, 1);
      uint32_t nxt = hdr + new_total;
      u_blk_set_size(uc, nxt, rest, 0);
      u_blk_set_prev(uc, nxt, new_total);
      uint32_t nn = nxt + rest;
      if (nn + U_HEAP_HDR <= u_heap_g.end)
        u_blk_set_prev(uc, nn, rest);
    }
    return p;
  }

  /* 尝试与后继空闲块合并以原地扩张 */
  {
    uint32_t nxt = hdr + old_total;
    if (nxt + U_HEAP_HDR <= u_heap_g.end) {
      uint32_t ns = u_rd32(uc, nxt);
      if (!(ns & U_FLAG_USED) && (ns & ~7u) >= U_BLK_MIN) {
        uint32_t merged = old_total + (ns & ~7u);
        if (merged >= need + U_HEAP_HDR) {
          uint32_t rest = merged - (need + U_HEAP_HDR);
          if (rest >= U_BLK_MIN) {
            u_blk_set_size(uc, hdr, need + U_HEAP_HDR, 1);
            uint32_t nn2 = hdr + need + U_HEAP_HDR;
            u_blk_set_size(uc, nn2, rest, 0);
            u_blk_set_prev(uc, nn2, need + U_HEAP_HDR);
            uint32_t tail = nn2 + rest;
            if (tail + U_HEAP_HDR <= u_heap_g.end)
              u_blk_set_prev(uc, tail, rest);
          } else {
            u_blk_set_size(uc, hdr, merged, 1);
            uint32_t tail = hdr + merged;
            if (tail + U_HEAP_HDR <= u_heap_g.end)
              u_blk_set_prev(uc, tail, merged);
          }
          return p;
        }
      }
    }
  }

  /* 兜底：新分配 + 拷贝 + 释放 */
  {
    uint32_t np = u_malloc(uc, size);
    if (!np)
      return 0;
    u_memcpy(uc, np, p, old_payload < need ? old_payload : need);
    u_free(uc, p);
    return np;
  }
}

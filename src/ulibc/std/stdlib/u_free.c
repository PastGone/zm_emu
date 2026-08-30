#include "../../include/u_heap.h"

#include "../../../log/log.h"
#include "../../include/internal/u_heap_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_free.c
 * @brief u_free —— 释放客户机堆块（与前后相邻空闲块合并）
 */

void u_free(uc_engine *uc, uint32_t p) {
  if (!u_heap_ready(uc) || p == 0)
    return;
  if (p < u_heap_g.base + U_HEAP_HDR || p >= u_heap_g.end)
    return;

  uint32_t hdr = p - U_HEAP_HDR;
  if ((hdr - u_heap_g.base) & 7u)
    return; /* 非 8 对齐 → 不是本堆的块，拒绝（防误 free） */

  uint32_t sz = u_blk_total(uc, hdr);
  if (sz < U_BLK_MIN || hdr + sz > u_heap_g.end) {
    log_warn("u_heap: free(0x%08X) 块头损坏，忽略", p);
    return;
  }
  if (!u_blk_used(uc, hdr)) {
    log_warn("u_heap: free(0x%08X) 重复释放，忽略", p);
    return;
  }

  uint32_t prev_total = u_rd32(uc, hdr + 4);
  uint32_t new_prev = prev_total;

  u_blk_set_size(uc, hdr, sz, 0);

  /* 向后合并 */
  {
    uint32_t nxt = hdr + sz;
    if (nxt + U_HEAP_HDR <= u_heap_g.end) {
      uint32_t ns = u_rd32(uc, nxt);
      if (!(ns & U_FLAG_USED) && (ns & ~7u) >= U_BLK_MIN)
        sz += (ns & ~7u);
    }
  }

  /* 向前合并 */
  if (prev_total >= U_BLK_MIN && hdr >= u_heap_g.base + prev_total) {
    uint32_t prev = hdr - prev_total;
    uint32_t ps = u_rd32(uc, prev);
    if (!(ps & U_FLAG_USED) && (ps & ~7u) >= U_BLK_MIN &&
        prev + (ps & ~7u) == hdr) {
      uint32_t prev_sz = ps & ~7u;
      new_prev = u_rd32(uc, prev + 4);
      hdr = prev;
      sz += prev_sz;
    }
  }

  u_blk_set_size(uc, hdr, sz, 0);
  u_blk_set_prev(uc, hdr, new_prev);

  /* 更新后继块的 prev_size */
  {
    uint32_t nn = hdr + sz;
    if (nn + U_HEAP_HDR <= u_heap_g.end)
      u_blk_set_prev(uc, nn, sz);
  }
}

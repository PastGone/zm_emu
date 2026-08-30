#include "../../include/u_heap.h"

#include "../../../log/log.h"
#include "../../include/internal/u_heap_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_malloc.c
 * @brief u_malloc —— 客户机堆分配（一级基本函数）
 *
 * 必须重写：宿主 malloc 返回宿主地址，客户机拿着它访问会落到未映射区间。
 * 策略：first-fit + 空闲块分裂。
 */

uint32_t u_malloc(uc_engine *uc, uint32_t size) {
  if (!u_heap_ready(uc) || size == 0)
    return 0;

  uint32_t need = u_heap_align8(size);
  if (need < U_HEAP_ALIGN)
    need = U_HEAP_ALIGN;
  uint32_t total = need + U_HEAP_HDR;

  uint32_t cur = u_heap_g.base;
  while (cur + U_HEAP_HDR <= u_heap_g.end) {
    uint32_t flags = u_rd32(uc, cur);
    uint32_t bsz = flags & ~7u;

    /* 头损坏或越界 → 停止遍历，防止死循环 */
    if (bsz < U_BLK_MIN || cur + bsz > u_heap_g.end)
      break;

    if (!(flags & U_FLAG_USED) && bsz >= total) {
      uint32_t rest = bsz - total;
      uint32_t nxt = cur + total;

      if (rest >= U_BLK_MIN) {
        /* 分裂：前一半分配出去，后一半留作新空闲块 */
        u_blk_set_size(uc, cur, total, 1);
        u_blk_set_size(uc, nxt, rest, 0);
        u_blk_set_prev(uc, nxt, total);
        if (nxt + rest + U_HEAP_HDR <= u_heap_g.end)
          u_blk_set_prev(uc, nxt + rest, rest);
      } else {
        /* 剩余不足以成块，整块分配（内部碎片） */
        u_blk_set_size(uc, cur, bsz, 1);
        if (cur + bsz + U_HEAP_HDR <= u_heap_g.end)
          u_blk_set_prev(uc, cur + bsz, bsz);
      }
      return cur + U_HEAP_HDR;
    }
    cur += bsz;
  }

  log_warn("u_heap: 内存不足，malloc(%u) 失败", size);
  return 0;
}

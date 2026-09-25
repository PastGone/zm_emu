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

  /* 诊断：异常大的请求把**调用点**打出来（阈值 ZM_MALLOC_DBG_KB，默认 1MB）。
   *
   * 起因：0000050b 一族在 init 里 `malloc(16581376)`（= 0xFD1000 ✗，恰好是我们
   * CBK 跳板页后一页）失败 → 拿到 NULL 后把"尺寸值"当指针用 → 后面所有写地址
   * 都带着 0xFD1000 的影子 → 写到映射外 err=7。
   * 那句 15.8MB 的请求在真机（闪存卡 + 小内存）上不合理，所以要看清是谁按什么
   * 算出来的：直接把 LR 打出来，对着反汇编就能找到调用点 ✓。 */
  {
    static int dbg_kb = -1;
    if (dbg_kb < 0) {
      const char *e = getenv("ZM_MALLOC_DBG_KB");
      dbg_kb = (e && atoi(e) >= 0) ? atoi(e) : 1024;
    }
    if (dbg_kb > 0 && size >= (uint32_t)dbg_kb * 1024u) {
      uint32_t lr = 0, sp = 0;
      uc_reg_read(uc, UC_ARM_REG_LR, &lr);
      uc_reg_read(uc, UC_ARM_REG_SP, &sp);
      log_warn("u_malloc 大请求：%u 字节 (0x%X)，调用点 LR=0x%X SP=0x%X", size,
               size, lr, sp);
    }
  }

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
      {
        uint32_t p = cur + U_HEAP_HDR;
        /* ★ 新分配的块**清零**（默认开，ZM_HEAP_REUSE=1 可关）。
         *
         * 为什么默认要清：真机上客户机堆来自 mmap 的**全零页** —— applet 里
         * 大量对象只是 `malloc` 后**只写部分字段**（其余依赖"新内存是 0"）。
         * 我们的堆是模拟的、会**复用刚 free 的块**，于是那些字段保留着上一个
         * 对象的残渣（实测是 GBK 文本、旧指针等 ✗），applet 一旦把它们当指针/
         * 尺寸用就野读野跳。
         *
         * 实测 00000462《三国情仇》：点"开始游戏"后崩点**每次都不同**
         * （0x34BF4 字体查表 / 0xFA9C 精灵 blit / 0x4148），字段值都是"重复两
         * 字节的文本残渣"✗ —— 清零后这些字段回到 0，走 applet 的"空/默认"分支。
         * 代价：每次 malloc 多一次 memset（客户机内存写，6MB 堆下可忽略）。 */
        static int zero_mode = -1;
        if (zero_mode < 0) {
          /* 默认**关**：试过对 00000462 无效（崩点不在回收块上），而它改变了
           * malloc 语义，所以保持选择性开启，避免影响其它 applet。 */
          const char *e = getenv("ZM_HEAP_ZERO");
          zero_mode = (e && e[0] == '1') ? 1 : 0;
          if (zero_mode)
            log_info("u_heap: 新分配块清零已开启（ZM_HEAP_ZERO=1）");
        }
        if (zero_mode) {
          /* 只清**请求的可用区**（need），不要用 bsz —— 分裂后 bsz 是"分裂前"
           * 的整块大小，会把下一个块的头部一起清掉 ✗。 */
          static const uint8_t z[256] = {0};
          for (uint32_t off = 0; off < need;) {
            uint32_t n = (need - off) < sizeof(z) ? (need - off)
                                                  : (uint32_t)sizeof(z);
            if (uc_mem_write(uc, p + off, z, n) != UC_ERR_OK)
              break;
            off += n;
          }
        }
        return p;
      }
    }
    cur += bsz;
  }

  log_warn("u_heap: 内存不足，malloc(%u) 失败", size);
  return 0;
}

#include "zm_mem.h"

#include "../../emu.h"
#include "../../log/log.h"
#include "../../tool/uc_helper.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 客户机堆分配器 ----------------
 *
 * 需求有两个，而且互相拉扯：
 *
 *  1) 必须支持 free 后复用。原来是"只涨不落"的 bump 指针、free 为 no-op，
 *     00000440 反复申请 / 释放 4MB 级资源缓冲，48MB 堆几秒就被吃光，
 *     之后所有分配返回 0，画面全黑。
 *
 *  2) 不能在客户机内存里插块头。这些老 applet 是照着 bump 分配器写的，
 *     隐含假设连续 malloc 出来的块在地址上首尾相接。中间一旦插进 8 字节
 *     块头，轻微越界访问就会读到块头而不是下一块的数据，行为立刻跑偏
 *     （00000506 的资源反序列化就是这么开始读野指针、陷入死循环的）。
 *
 * 所以记账全部放在宿主机侧：一张 addr -> size 的开放寻址哈希表 +
 * 一个空闲块列表；客户机看到的内存布局与纯 bump 分配器完全一致。
 */

#define ZM_BLK_CAP 65536 /* 哈希表槽位，必须是 2 的幂 */
#define ZM_FREE_CAP 16384

typedef struct {
  uint32_t addr; /* 0 = 空槽 */
  uint32_t size;
} Blk;

static Blk g_blk[ZM_BLK_CAP];
static uint32_t g_blk_n = 0;

typedef struct {
  uint32_t addr;
  uint32_t size;
} FreeBlk;

static FreeBlk g_free[ZM_FREE_CAP];
static int g_free_n = 0;
static uint32_t g_peak = 0;

static inline uint32_t blk_hash(uint32_t a) {
  return (a * 2654435761u) & (ZM_BLK_CAP - 1);
}

static void blk_put(uint32_t addr, uint32_t size) {
  if (g_blk_n >= ZM_BLK_CAP / 2)
    return; /* 表过半就不再记账，退化成"该块不可复用" */
  uint32_t h = blk_hash(addr);
  while (g_blk[h].addr && g_blk[h].addr != addr)
    h = (h + 1) & (ZM_BLK_CAP - 1);
  if (!g_blk[h].addr)
    g_blk_n++;
  g_blk[h].addr = addr;
  g_blk[h].size = size;
}

static uint32_t blk_get(uint32_t addr) {
  uint32_t h = blk_hash(addr);
  uint32_t probes = 0;
  while (g_blk[h].addr && probes++ < ZM_BLK_CAP) {
    if (g_blk[h].addr == addr)
      return g_blk[h].size;
    h = (h + 1) & (ZM_BLK_CAP - 1);
  }
  return 0;
}

uint32_t host_malloc(uint32_t *heap_ptr, uint32_t size) {
  if (size == 0)
    size = 4;
  size = (size + 7u) & ~7u;

  /* 1) 空闲表里找最贴合的一块 */
  int best = -1;
  for (int i = 0; i < g_free_n; i++) {
    if (g_free[i].size < size)
      continue;
    if (best < 0 || g_free[i].size < g_free[best].size)
      best = i;
  }
  if (best >= 0) {
    uint32_t p = g_free[best].addr;
    g_free[best] = g_free[--g_free_n];
    log_debug("[HEAP] 复用空闲块 %u 字节 @0x%08X", size, p);
    return p;
  }

  /* 2) 从 bump 区切一块（客户机侧无块头，地址保持连续） */
  if (*heap_ptr + size > HEAP_END || *heap_ptr + size < *heap_ptr) {
    log_error("客户机堆耗尽：请求 %u 字节，已用 %u/%u KB（空闲块 %d 个）", size,
              (*heap_ptr - HEAP_BASE) / 1024, HEAP_SIZE / 1024, g_free_n);
    return 0;
  }

  uint32_t p = *heap_ptr;
  *heap_ptr += size;
  if (*heap_ptr - HEAP_BASE > g_peak)
    g_peak = *heap_ptr - HEAP_BASE;

  blk_put(p, size);
  log_debug("[HEAP] 分配 %u 字节 @0x%08X（已用 %u KB）", size, p,
            (*heap_ptr - HEAP_BASE) / 1024);
  return p;
}

void host_free(uint32_t ptr) {
  if (!ptr)
    return;
  if (getenv("ZM_NO_HEAP_REUSE"))
    return;
  if (ptr < HEAP_BASE || ptr >= HEAP_END)
    return; /* 不是堆地址（可能是 shim 对象），忽略 */

  uint32_t size = blk_get(ptr);
  if (size == 0)
    return; /* 没记账过 / 不是块首，安全起见不回收 */

  for (int i = 0; i < g_free_n; i++)
    if (g_free[i].addr == ptr)
      return; /* 重复 free */

  if (g_free_n >= ZM_FREE_CAP)
    return;

  g_free[g_free_n].addr = ptr;
  g_free[g_free_n].size = size;
  g_free_n++;
  log_debug("[HEAP] 回收 %u 字节 @0x%08X（空闲块 %d）", size, ptr, g_free_n);
}

uint32_t host_heap_used(uint32_t heap_ptr) { return heap_ptr - HEAP_BASE; }
uint32_t host_heap_peak(void) { return g_peak; }

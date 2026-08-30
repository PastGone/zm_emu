#include "../../include/u_stdlib.h"

/**
 * @file u_bsearch_host.c
 * @brief u_bsearch_host —— 二分查找（比较器在宿主侧）
 *
 * 标准 bsearch 的比较器是客户机代码地址，需嵌套 uc_emu_start 回调；
 * 本变体接受宿主侧比较器，覆盖 trap 场景。
 */

uint32_t u_bsearch_host(uc_engine *uc, uint32_t key, uint32_t base,
                        uint32_t n, uint32_t width, u_compare_fn cmp) {
  if (!uc || !cmp || n == 0 || width == 0 || base == 0)
    return 0;

  uint32_t lo = 0;
  uint32_t hi = n;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2u;
    uint32_t addr = base + mid * width;
    int r = cmp(uc, key, addr);
    if (r == 0)
      return addr;
    if (r < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return 0;
}

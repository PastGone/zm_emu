#include "../../include/u_mem.h"

#include <string.h>

/**
 * @file u_memcmp.c
 * @brief u_memcmp —— 比较两块客户机内存（一级基本函数）
 */

int u_memcmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n) {
  if (!uc || n == 0)
    return 0;
  if (a == b)
    return 0;

  uint8_t ba[U_MEM_CHUNK];
  uint8_t bb[U_MEM_CHUNK];
  uint32_t done = 0;
  while (done < n) {
    uint32_t chunk = n - done;
    if (chunk > (uint32_t)sizeof(ba))
      chunk = (uint32_t)sizeof(ba);
    if (!u_read(uc, a + done, ba, chunk))
      return -1; /* 读失败：无法比较，按"小于"处理，避免误判相等 */
    if (!u_read(uc, b + done, bb, chunk))
      return 1;
    int r = memcmp(ba, bb, chunk);
    if (r != 0)
      return r;
    done += chunk;
  }
  return 0;
}

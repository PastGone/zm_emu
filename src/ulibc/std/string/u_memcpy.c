#include "../../include/u_mem.h"

/**
 * @file u_memcpy.c
 * @brief u_memcpy —— 客户机内存块复制（一级基本函数）
 */

uint32_t u_memcpy(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n) {
  if (!uc || n == 0)
    return 0;
  if (dst == src)
    return n;

  uint8_t buf[U_MEM_CHUNK];
  uint32_t done = 0;
  while (done < n) {
    uint32_t chunk = n - done;
    if (chunk > (uint32_t)sizeof(buf))
      chunk = (uint32_t)sizeof(buf);
    if (!u_read(uc, src + done, buf, chunk))
      break;
    if (!u_write(uc, dst + done, buf, chunk))
      break;
    done += chunk;
  }
  return done;
}

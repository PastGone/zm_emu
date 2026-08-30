#include "../../include/u_mem.h"

/**
 * @file u_strnlen.c
 * @brief u_strnlen —— 最多扫 maxlen 字节的 C 串长度（POSIX / C11）
 */

uint32_t u_strnlen(uc_engine *uc, uint32_t s, uint32_t maxlen) {
  if (!uc || s == 0 || maxlen == 0)
    return 0;

  uint8_t buf[64];
  uint32_t n = 0;
  while (n < maxlen) {
    uint32_t chunk = maxlen - n;
    if (chunk > (uint32_t)sizeof(buf))
      chunk = (uint32_t)sizeof(buf);
    if (!u_read(uc, s + n, buf, chunk)) {
      for (; n < maxlen; n++) {
        if (!u_read(uc, s + n, buf, 1))
          return n;
        if (buf[0] == 0)
          return n;
      }
      return n;
    }
    for (uint32_t i = 0; i < chunk; i++) {
      if (buf[i] == 0)
        return n + i;
    }
    n += chunk;
  }
  return maxlen;
}

#include "../../include/u_str.h"

#include "../../include/u_mem.h"

/**
 * @file u_strrchr.c
 * @brief u_strrchr —— 查找字符最后一次出现
 */

uint32_t u_strrchr(uc_engine *uc, uint32_t s, int c) {
  if (!uc || s == 0)
    return 0;
  uint32_t len = u_strlen(uc, s);
  if ((c & 0xFF) == 0)
    return s + len;

  /* 逐块反向扫描 */
  uint8_t needle = (uint8_t)(c & 0xFF);
  uint8_t buf[64];
  uint32_t done = 0;
  while (done < len) {
    uint32_t chunk = len - done;
    if (chunk > (uint32_t)sizeof(buf))
      chunk = (uint32_t)sizeof(buf);
    uint32_t off = len - done - chunk;
    if (!u_read(uc, s + off, buf, chunk))
      break;
    uint32_t i = chunk;
    while (i > 0) {
      i--;
      if (buf[i] == needle)
        return s + off + i;
    }
    done += chunk;
  }
  return 0;
}

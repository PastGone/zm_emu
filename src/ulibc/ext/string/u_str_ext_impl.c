
#include <ctype.h>

#include "../../include/internal/u_str_ext_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_str_ext_impl.c
 * @brief 字符串非标准扩展的共享实现（非公开函数）
 */

int u_icmp_impl(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n,
                int bounded) {
  uint32_t la = bounded ? u_strnlen(uc, a, n) : u_strlen(uc, a);
  uint32_t lb = bounded ? u_strnlen(uc, b, n) : u_strlen(uc, b);
  uint32_t m = (la < lb) ? la : lb;

  uint8_t ba[64];
  uint8_t bb[64];
  uint32_t done = 0;
  while (done < m) {
    uint32_t c = m - done;
    if (c > (uint32_t)sizeof(ba))
      c = (uint32_t)sizeof(ba);
    if (!u_read(uc, a + done, ba, c))
      break;
    if (!u_read(uc, b + done, bb, c))
      break;
    for (uint32_t i = 0; i < c; i++) {
      int x = tolower((unsigned char)ba[i]);
      int y = tolower((unsigned char)bb[i]);
      if (x != y)
        return x - y;
    }
    done += c;
  }
  if (la == lb)
    return 0;
  return (la < lb) ? -1 : 1;
}

uint32_t u_xlate_case(uc_engine *uc, uint32_t s, int upper) {
  if (!uc || s == 0)
    return s;
  uint32_t len = u_strlen(uc, s);
  uint8_t buf[64];
  for (uint32_t off = 0; off < len;) {
    uint32_t c = len - off;
    if (c > (uint32_t)sizeof(buf))
      c = (uint32_t)sizeof(buf);
    if (!u_read(uc, s + off, buf, c))
      break;
    for (uint32_t i = 0; i < c; i++)
      buf[i] = (uint8_t)(upper ? toupper((unsigned char)buf[i])
                               : tolower((unsigned char)buf[i]));
    if (!u_write(uc, s + off, buf, c))
      break;
    off += c;
  }
  return s;
}

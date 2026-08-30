#include "../../include/u_str_ext.h"

#include <stdlib.h>
#include <string.h>

#include "../../include/u_mem.h"
#include "../../include/u_mem_ext.h"

/**
 * @file u_strstr_host.c
 * @brief u_strstr_host —— 子串查找，needle 是宿主常串（非标准便利函数）
 */

uint32_t u_strstr_host(uc_engine *uc, uint32_t hay, const char *needle) {
  if (!uc || hay == 0 || !needle)
    return 0;
  size_t nlen = strlen(needle);
  if (nlen == 0)
    return hay;
  uint32_t hlen = u_strlen(uc, hay);
  if (nlen > (size_t)hlen)
    return 0;

  /* 短串：整块抓到宿主内存后用宿主 strstr */
  if (hlen <= 65536u) {
    char *hs = (char *)malloc((size_t)hlen + 1u);
    if (!hs)
      return 0;
    u_read(uc, hay, hs, hlen);
    hs[hlen] = '\0';
    char *hit = strstr(hs, needle);
    uint32_t r = hit ? hay + (uint32_t)(hit - hs) : 0;
    free(hs);
    return r;
  }
  /* 超长串：首字符过滤 + 逐位比较 */
  uint8_t first = (uint8_t)needle[0];
  uint32_t limit = hlen - (uint32_t)nlen;
  for (uint32_t i = 0; i <= limit; i++) {
    if (u_rd8(uc, hay + i) != first)
      continue;
    if (u_memcmp_host(uc, hay + i, needle, (uint32_t)nlen) == 0)
      return hay + i;
  }
  return 0;
}

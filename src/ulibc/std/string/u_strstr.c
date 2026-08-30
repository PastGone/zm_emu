#include "../../include/u_str.h"

#include <stdlib.h>
#include <string.h>

#include "../../include/u_mem.h"

/**
 * @file u_strstr.c
 * @brief u_strstr —— 子串查找
 *
 * 短串（<=64KB）整块抓到宿主内存后用宿主 strstr；超长串退化为
 * 首字符过滤 + 逐位比较。
 */

#define U_FETCH_MAX 65536u

/** 把客户机串抓到宿主缓冲；超长时 malloc，*heap 标记是否需要 free */
static char *fetch_str(uc_engine *uc, uint32_t addr, uint32_t len,
                       char *stackbuf, size_t stackcap, int *heap) {
  *heap = 0;
  if (addr == 0 || len == 0) {
    if (stackcap)
      stackbuf[0] = '\0';
    return stackbuf;
  }
  if ((size_t)len + 1u <= stackcap) {
    u_read(uc, addr, stackbuf, len);
    stackbuf[len] = '\0';
    return stackbuf;
  }
  char *p = (char *)malloc((size_t)len + 1u);
  if (!p) {
    stackbuf[0] = '\0';
    return stackbuf;
  }
  u_read(uc, addr, p, len);
  p[len] = '\0';
  *heap = 1;
  return p;
}

uint32_t u_strstr(uc_engine *uc, uint32_t hay, uint32_t needle) {
  if (!uc || hay == 0)
    return 0;
  uint32_t nlen = u_strlen(uc, needle);
  if (nlen == 0)
    return hay;
  uint32_t hlen = u_strlen(uc, hay);
  if (nlen > hlen)
    return 0;

  char nstack[256];
  int nheap = 0;
  char *ns = fetch_str(uc, needle, nlen, nstack, sizeof(nstack), &nheap);

  uint32_t result = 0;
  if (hlen <= U_FETCH_MAX) {
    char *hs = (char *)malloc((size_t)hlen + 1u);
    if (hs) {
      u_read(uc, hay, hs, hlen);
      hs[hlen] = '\0';
      char *hit = strstr(hs, ns);
      if (hit)
        result = hay + (uint32_t)(hit - hs);
      free(hs);
    }
  } else {
    uint8_t first = (uint8_t)ns[0];
    uint32_t limit = hlen - nlen;
    for (uint32_t i = 0; i <= limit; i++) {
      if (u_rd8(uc, hay + i) != first)
        continue;
      if (u_memcmp(uc, hay + i, needle, nlen) == 0) {
        result = hay + i;
        break;
      }
    }
  }

  if (nheap)
    free(ns);
  return result;
}

#include "../../include/u_str_ext.h"

#include <string.h>

#include "../../include/u_mem.h"
#include "../../include/u_mem_ext.h"

/**
 * @file u_strcmp_host.c
 * @brief u_strcmp_host / u_strncmp_host —— 客户机串 vs 宿主常串
 *
 * 非标准：模拟器便利函数。拿 applet 传进来的路径直接和宿主常量比较时
 * 最省事，省去"先抓到宿主缓冲再比"。
 */

int u_strcmp_host(uc_engine *uc, uint32_t a, const char *b) {
  if (!uc)
    return 0;
  if (!b)
    return 1;
  uint32_t la = u_strlen(uc, a);
  uint32_t lb = (uint32_t)strlen(b);
  uint32_t m = (la < lb) ? la : lb;
  if (m) {
    int r = u_memcmp_host(uc, a, b, m);
    if (r)
      return r;
  }
  if (la == lb)
    return 0;
  return (la < lb) ? -1 : 1;
}

int u_strncmp_host(uc_engine *uc, uint32_t a, const char *b, uint32_t n) {
  if (!uc || n == 0)
    return 0;
  if (!b)
    return 1;
  uint32_t la = u_strnlen(uc, a, n);
  uint32_t lb = (uint32_t)strlen(b);
  if (lb > n)
    lb = n;
  uint32_t m = (la < lb) ? la : lb;
  if (m) {
    int r = u_memcmp_host(uc, a, b, m);
    if (r)
      return r;
  }
  if (la == lb)
    return 0;
  return (la < lb) ? -1 : 1;
}

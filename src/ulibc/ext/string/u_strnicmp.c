#include "../../include/u_str_ext.h"

#include "../../include/internal/u_str_ext_impl.h"

/**
 * @file u_strnicmp.c
 * @brief u_strnicmp —— 限定长度的大小写不敏感比较（对应 zmaee_strnicmp）
 *
 * 非标准。与 u_stricmp 共用 u_icmp_impl。
 */

int u_strnicmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n) {
  if (!uc || n == 0 || a == b)
    return 0;
  return u_icmp_impl(uc, a, b, n, 1);
}

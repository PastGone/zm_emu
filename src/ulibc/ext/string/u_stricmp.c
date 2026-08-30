#include "../../include/u_str_ext.h"

#include "../../include/internal/u_str_ext_impl.h"

/**
 * @file u_stricmp.c
 * @brief u_stricmp —— 大小写不敏感比较（对应 zmaee_stricmp）
 *
 * 非标准。与 u_strnicmp 共用 u_icmp_impl。
 */

int u_stricmp(uc_engine *uc, uint32_t a, uint32_t b) {
  if (!uc || a == b)
    return 0;
  return u_icmp_impl(uc, a, b, 0, 0);
}

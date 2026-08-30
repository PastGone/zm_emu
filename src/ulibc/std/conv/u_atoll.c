#include "../../include/u_stdlib.h"

/**
 * @file u_atoll.c
 * @brief u_atoll —— 简写转换（依赖 u_strtoll_ex）
 */

long long u_atoll(uc_engine *uc, uint32_t s) {
  return u_strtoll_ex(uc, s, NULL, 10);
}

#include "../../include/u_stdlib.h"

/**
 * @file u_atoi.c
 * @brief u_atoi —— 简写转换（依赖 u_strtol_ex）
 */

int u_atoi(uc_engine *uc, uint32_t s) {
  return (int)u_strtol_ex(uc, s, NULL, 10);
}

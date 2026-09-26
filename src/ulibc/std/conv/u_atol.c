#include "../../include/u_stdlib.h"

/**
 * @file u_atol.c
 * @brief u_atol —— 简写转换（依赖 u_strtol_ex）
 */

long u_atol(uc_engine *uc, uint32_t s) {
	return u_strtol_ex(uc, s, NULL, 10);
}

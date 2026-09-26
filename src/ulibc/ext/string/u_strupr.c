#include "../../include/u_str_ext.h"

#include "../../include/internal/u_str_ext_impl.h"

/**
 * @file u_strupr.c
 * @brief u_strupr —— 原地转大写（对应 zmaee_strupr，非标准）
 */

uint32_t u_strupr(uc_engine *uc, uint32_t s) {
	return u_xlate_case(uc, s, 1);
}

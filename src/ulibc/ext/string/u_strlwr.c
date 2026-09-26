#include "../../include/u_str_ext.h"

#include "../../include/internal/u_str_ext_impl.h"

/**
 * @file u_strlwr.c
 * @brief u_strlwr —— 原地转小写（对应 zmaee_strlwr，非标准）
 */

uint32_t u_strlwr(uc_engine *uc, uint32_t s) {
	return u_xlate_case(uc, s, 0);
}

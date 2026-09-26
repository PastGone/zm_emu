#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wmemcpy.c
 * @brief u_wmemcpy —— 按宽字符计数的块复制（依赖 u_memcpy）
 */

uint32_t u_wmemcpy(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n) {
	if (!uc || n == 0)
		return dst;
	u_memcpy(uc, dst, src, n * 2u);
	return dst;
}

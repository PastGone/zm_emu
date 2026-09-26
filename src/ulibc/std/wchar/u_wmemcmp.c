#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wmemcmp —— 按宽字符比较（依赖 u_memcmp）
 */

int u_wmemcmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n) {
	if (!uc || n == 0)
		return 0;
	if (a == b)
		return 0;
	return u_memcmp(uc, a, b, n * 2u);
}

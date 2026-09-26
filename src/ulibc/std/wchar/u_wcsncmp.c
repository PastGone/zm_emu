#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcsncmp.c
 * @brief u_wcsncmp —— 限定长度的宽串比较
 */

int u_wcsncmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n) {
	if (!uc || n == 0)
		return 0;
	if (a == b)
		return 0;

	for (uint32_t i = 0; i < n; i++) {
		uint16_t x = u_rd16(uc, a + i * 2u);
		uint16_t y = u_rd16(uc, b + i * 2u);
		if (x != y)
			return (int)x - (int)y;
		if (x == 0)
			return 0; /* 同时到达终止符 */
	}
	return 0;
}

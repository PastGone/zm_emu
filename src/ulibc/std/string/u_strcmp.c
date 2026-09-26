#include "../../include/u_str.h"

#include "../../include/u_mem.h"

/**
 * @file u_strcmp.c
 * @brief u_strcmp —— 比较两串（依赖 u_strlen / u_memcmp）
 */

int u_strcmp(uc_engine *uc, uint32_t a, uint32_t b) {
	if (!uc)
		return 0;
	if (a == b)
		return 0;
	uint32_t la = u_strlen(uc, a);
	uint32_t lb = u_strlen(uc, b);
	uint32_t m = (la < lb) ? la : lb;
	if (m) {
		int r = u_memcmp(uc, a, b, m);
		if (r)
			return r;
	}
	if (la == lb)
		return 0;
	return (la < lb) ? -1 : 1;
}

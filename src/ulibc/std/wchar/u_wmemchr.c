#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wmemchr.c
 * @brief u_wmemchr —— 在宽字符块中查找字符，返回客户机地址
 */

uint32_t u_wmemchr(uc_engine *uc, uint32_t s, u_wchar c, uint32_t n) {
	if (!uc || n == 0)
		return 0;
	for (uint32_t i = 0; i < n; i++) {
		if (u_rd16(uc, s + i * 2u) == c)
			return s + i * 2u;
	}
	return 0;
}

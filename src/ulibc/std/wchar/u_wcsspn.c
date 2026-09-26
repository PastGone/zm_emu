#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcsspn.c
 * @brief u_wcsspn —— 完全由 accept 中字符组成的前缀长度（单位：宽字符）
 */

uint32_t u_wcsspn(uc_engine *uc, uint32_t s, uint32_t accept) {
	if (!uc || s == 0)
		return 0;
	uint32_t nlen = u_wcslen(uc, accept);
	uint32_t len = u_wcslen(uc, s);

	for (uint32_t i = 0; i < len; i++) {
		uint16_t c = u_rd16(uc, s + i * 2u);
		uint32_t j = 0;
		for (; j < nlen; j++) {
			if (u_rd16(uc, accept + j * 2u) == c)
				break;
		}
		if (j == nlen)
			return i;
	}
	return len;
}

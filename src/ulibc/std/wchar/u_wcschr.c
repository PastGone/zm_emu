#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcschr.c
 * @brief u_wcschr —— 在 UCS-2 宽串中查找宽字符
 */

uint32_t u_wcschr(uc_engine *uc, uint32_t s, u_wchar c) {
	if (!uc || s == 0)
		return 0;
	for (uint32_t i = 0;; i++) {
		uint16_t w = u_rd16(uc, s + i * 2u);
		if (w == c)
			return s + i * 2u;
		if (w == 0)
			return 0;
	}
}

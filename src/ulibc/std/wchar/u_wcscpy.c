#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcscpy.c
 * @brief u_wcscpy —— 拷贝 UCS-2 宽串（依赖 u_wcslen / u_memcpy）
 */

uint32_t u_wcscpy(uc_engine *uc, uint32_t dst, uint32_t src) {
	if (!uc || dst == 0)
		return dst;
	if (src == 0) {
		u_wr16(uc, dst, 0);
		return dst;
	}
	uint32_t len = u_wcslen(uc, src);
	u_memcpy(uc, dst, src, len * 2u);
	u_wr16(uc, dst + len * 2u, 0);
	return dst;
}

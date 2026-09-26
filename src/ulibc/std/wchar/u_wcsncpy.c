#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcsncpy.c
 * @brief u_wcsncpy —— 限定长度的宽串拷贝
 *
 * 与标准一致：源串短于 n 时补 0 填满 n；拷满 n 则不以 '\0' 结尾。
 */

uint32_t u_wcsncpy(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n) {
	if (!uc || dst == 0 || n == 0)
		return dst;
	if (src == 0) {
		u_memset(uc, dst, 0, n * 2u);
		return dst;
	}

	/* 先扫出 src 在前 n 个宽字符内的实际长度 */
	uint32_t len = 0;
	while (len < n && u_rd16(uc, src + len * 2u) != 0)
		len++;

	if (len)
		u_memcpy(uc, dst, src, len * 2u);
	if (len < n)
		u_memset(uc, dst + len * 2u, 0, (n - len) * 2u);
	return dst;
}

#include "../../include/u_str.h"

#include "../../include/u_mem.h"

/**
 * @file u_strncat.c
 * @brief u_strncat —— 限定长度的追加，并补 '\0'
 */

uint32_t u_strncat(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n) {
	if (!uc || dst == 0 || src == 0 || n == 0)
		return dst;
	uint32_t dlen = u_strlen(uc, dst);
	uint32_t slen = u_strnlen(uc, src, n);
	u_memcpy(uc, dst + dlen, src, slen);
	u_wr8(uc, dst + dlen + slen, 0);
	return dst;
}

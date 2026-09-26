#include "../../include/u_str.h"

#include "../../include/u_mem.h"

/**
 * @file u_strcat.c
 * @brief u_strcat —— 追加客户机 C 串（依赖 u_strlen / u_memcpy）
 */

uint32_t u_strcat(uc_engine *uc, uint32_t dst, uint32_t src) {
	if (!uc || dst == 0 || src == 0)
		return dst;
	uint32_t dlen = u_strlen(uc, dst);
	uint32_t slen = u_strlen(uc, src);
	u_memcpy(uc, dst + dlen, src, slen);
	u_wr8(uc, dst + dlen + slen, 0);
	return dst;
}

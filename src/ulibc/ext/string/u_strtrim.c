#include "../../include/u_str_ext.h"

#include <ctype.h>

#include "../../include/u_mem.h"

/**
 * @file u_strtrim.c
 * @brief u_strtrim —— 原地去除首尾空白（对应 zmaee_strtrim，非标准）
 */

uint32_t u_strtrim(uc_engine *uc, uint32_t s) {
	if (!uc || s == 0)
		return s;
	uint32_t len = u_strlen(uc, s);
	uint32_t b = 0;
	while (b < len) {
		uint8_t c = u_rd8(uc, s + b);
		if (c == 0 || !isspace((unsigned char)c))
			break;
		b++;
	}
	uint32_t e = len;
	while (e > b) {
		uint8_t c = u_rd8(uc, s + e - 1);
		if (!isspace((unsigned char)c))
			break;
		e--;
	}
	if (b > 0 && e > b)
		u_memmove(uc, s, s + b, e - b);
	u_wr8(uc, s + (e - b), 0);
	return s;
}

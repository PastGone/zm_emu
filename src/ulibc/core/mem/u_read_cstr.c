#include "../../include/u_mem.h"

/**
 * @file u_read_cstr.c
 * @brief u_read_cstr —— 客户机 C 串读入宿主缓冲
 */

uint32_t u_read_cstr(uc_engine *uc, uint32_t addr, char *buf, size_t cap) {
	if (!buf || cap == 0)
		return 0;
	buf[0] = '\0';
	if (!uc || addr == 0)
		return 0;

	size_t i = 0;
	while (i < cap - 1) {
		uint8_t c;
		if (!u_read(uc, addr + i, &c, 1))
			break;
		buf[i++] = (char)c;
		if (c == 0)
			return (uint32_t)(i - 1);
	}
	buf[i] = '\0';
	return (uint32_t)i;
}

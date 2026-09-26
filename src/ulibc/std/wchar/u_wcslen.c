#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcslen.c
 * @brief u_wcslen —— UCS-2 宽串长度（单位：字符）
 *
 * 客户机 wchar_t 为 2 字节，宿主 Linux 上为 4 字节，
 * 故不能转发宿主 wcslen，必须按 2 字节元素读取。
 */

uint32_t u_wcslen(uc_engine *uc, uint32_t s) {
	if (!uc || s == 0)
		return 0;
	uint32_t n = 0;
	for (;;) {
		uint16_t w = u_rd16(uc, s + n * 2u);
		if (w == 0)
			return n;
		/* 读到不可访问内存则终止，避免死循环 */
		uint8_t probe;
		if (!u_read(uc, s + n * 2u, &probe, 1))
			return n;
		n++;
	}
}

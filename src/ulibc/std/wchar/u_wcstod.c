#include "../../include/u_wcs.h"

#include <stdlib.h>

#include "../../include/u_mem.h"

/**
 * @file u_wcstod.c
 * @brief u_wcstod —— 宽串转 double
 *
 * 与 u_wcstol 同样：按 Latin-1 折叠成窄串后交给宿主 strtod。
 */

#define U_WCONV_WIN 512u

double u_wcstod(uc_engine *uc, uint32_t s, uint32_t endptr_addr) {
	char buf[U_WCONV_WIN];
	if (!uc || s == 0)
		return 0.0;

	uint32_t i = 0;
	while (i < U_WCONV_WIN - 1) {
		uint16_t c = u_rd16(uc, s + i * 2u);
		if (c == 0)
			break;
		buf[i] = (char)(c & 0xFF);
		i++;
	}
	buf[i] = '\0';

	char *e = NULL;
	double v = strtod(buf, &e);
	if (endptr_addr)
		u_wr32(uc, endptr_addr, s + (uint32_t)(e - buf) * 2u);
	return v;
}

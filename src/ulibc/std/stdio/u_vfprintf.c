#include "../../include/u_fmt.h"

#include <stdlib.h>

/**
 * @file u_vfprintf.c
 * @brief u_vfprintf —— 格式化输出到宿主 FILE*
 */

int u_vfprintf(uc_engine *uc, FILE *fp, uint32_t fmt, u_va *va) {
	if (!uc || !fp)
		return 0;

	char fmtbuf[U_FMT_MAX_FMT];
	u_read_cstr(uc, fmt, fmtbuf, sizeof(fmtbuf));

	size_t alloc = U_FMT_MAX_OUT + 1;
	char *buf = (char *)malloc(alloc);
	if (!buf)
		return 0;

	int n = u_vsnprintf_core(uc, va, buf, alloc, fmtbuf);
	fputs(buf, fp);
	free(buf);
	return n;
}

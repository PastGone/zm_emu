#include "../../include/u_stdio.h"

#include <stdlib.h>
#include <string.h>

#include "../../include/internal/u_stdio_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_fgets.c
 * @brief u_fgets —— 读一行到客户机缓冲区（含 '\n'，最多 n-1 字符）
 */

uint32_t u_fgets(uc_engine *uc, uint32_t dst, int n, uint32_t fp) {
	FILE *f = u_stdio_slot(fp);
	if (!f || dst == 0 || n <= 0)
		return 0;

	char *buf = (char *)malloc((size_t)n + 1u);
	if (!buf)
		return 0;
	if (!fgets(buf, n, f)) {
		free(buf);
		return 0;
	}
	size_t len = strlen(buf);
	u_write(uc, dst, buf, len + 1);
	free(buf);
	return dst;
}

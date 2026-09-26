#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_ungetc.c
 * @brief u_ungetc —— 把字符退回流的读缓冲区
 */

int u_ungetc(uc_engine *uc, uint32_t fp, int c) {
	(void)uc;
	FILE *f = u_stdio_slot(fp);
	return f ? ungetc(c, f) : EOF;
}

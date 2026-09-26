#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_fgetc.c
 * @brief u_fgetc / u_fputc —— 单字符读写（不触碰客户机内存）
 */

int u_fgetc(uc_engine *uc, uint32_t fp) {
	(void)uc;
	FILE *f = u_stdio_slot(fp);
	return f ? fgetc(f) : EOF;
}

int u_fputc(uc_engine *uc, uint32_t fp, int c) {
	(void)uc;
	FILE *f = u_stdio_slot(fp);
	return f ? fputc(c, f) : EOF;
}

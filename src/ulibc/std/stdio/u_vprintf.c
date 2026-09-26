#include "../../include/u_fmt.h"

/**
 * @file u_vprintf.c
 * @brief u_vprintf / u_printf —— 格式化输出到宿主 stdout
 */

int u_vprintf(uc_engine *uc, uint32_t fmt, u_va *va) {
	return u_vfprintf(uc, stdout, fmt, va);
}

int u_printf(uc_engine *uc, uint32_t fmt, u_va *va) {
	return u_vfprintf(uc, stdout, fmt, va);
}

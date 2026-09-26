#include "../../include/u_fmt.h"

/**
 * @file u_sprintf.c
 * @brief u_sprintf —— 无长度限制的格式化（依赖 u_vsnprintf）
 *
 * 内部按 U_FMT_MAX_OUT 防呆，避免客户机传入畸大格式串耗尽宿主内存。
 */

int u_sprintf(uc_engine *uc, uint32_t dst, uint32_t fmt, u_va *va) {
	return u_vsnprintf(uc, dst, U_FMT_MAX_OUT, fmt, va);
}

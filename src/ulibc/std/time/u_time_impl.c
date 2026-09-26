
#include <string.h>

#include "../../include/internal/u_time_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_time_impl.c
 * @brief struct tm 的客户机/宿主双向搬运（非公开函数）
 */

void u_tm_store(uc_engine *uc, uint32_t tm_ptr, const struct tm *t) {
	if (!uc || tm_ptr == 0 || !t)
		return;
	u_wr32(uc, tm_ptr + 0, (uint32_t)t->tm_sec);
	u_wr32(uc, tm_ptr + 4, (uint32_t)t->tm_min);
	u_wr32(uc, tm_ptr + 8, (uint32_t)t->tm_hour);
	u_wr32(uc, tm_ptr + 12, (uint32_t)t->tm_mday);
	u_wr32(uc, tm_ptr + 16, (uint32_t)t->tm_mon);
	u_wr32(uc, tm_ptr + 20, (uint32_t)t->tm_year);
	u_wr32(uc, tm_ptr + 24, (uint32_t)t->tm_wday);
	u_wr32(uc, tm_ptr + 28, (uint32_t)t->tm_yday);
	u_wr32(uc, tm_ptr + 32, (uint32_t)t->tm_isdst);
}

void u_tm_load(uc_engine *uc, uint32_t tm_ptr, struct tm *t) {
	memset(t, 0, sizeof(*t));
	if (!uc || tm_ptr == 0)
		return;
	t->tm_sec = (int)u_rd32(uc, tm_ptr + 0);
	t->tm_min = (int)u_rd32(uc, tm_ptr + 4);
	t->tm_hour = (int)u_rd32(uc, tm_ptr + 8);
	t->tm_mday = (int)u_rd32(uc, tm_ptr + 12);
	t->tm_mon = (int)u_rd32(uc, tm_ptr + 16);
	t->tm_year = (int)u_rd32(uc, tm_ptr + 20);
	t->tm_wday = (int)u_rd32(uc, tm_ptr + 24);
	t->tm_yday = (int)u_rd32(uc, tm_ptr + 28);
	t->tm_isdst = (int)u_rd32(uc, tm_ptr + 32);
}

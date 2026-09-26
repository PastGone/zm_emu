#include "../../include/u_mem.h"

/**
 * @file u_read.c
 * @brief u_read —— 从客户机内存读取（第 0 层原语）
 */

bool u_read(uc_engine *uc, uint32_t addr, void *buf, size_t len) {
	if (!uc || !buf)
		return false;
	if (len == 0)
		return true;
	return uc_mem_read(uc, (uint64_t)addr, buf, len) == UC_ERR_OK;
}

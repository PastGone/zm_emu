#include "../../include/u_stdio.h"

#include "../../include/u_mem.h"

/**
 * @file u_remove.c
 * @brief u_remove —— 删除文件（路径为客户机地址）
 */

int u_remove(uc_engine *uc, uint32_t path) {
	char pbuf[1024];
	if (!u_read_cstr(uc, path, pbuf, sizeof(pbuf)))
		return -1;
	return remove(pbuf);
}

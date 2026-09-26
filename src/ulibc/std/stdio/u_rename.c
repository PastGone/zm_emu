#include "../../include/u_stdio.h"

#include "../../include/u_mem.h"

/**
 * @file u_rename.c
 * @brief u_rename —— 重命名文件（两个路径均为客户机地址）
 */

int u_rename(uc_engine *uc, uint32_t oldp, uint32_t newp) {
	char obuf[1024];
	char nbuf[1024];
	if (!u_read_cstr(uc, oldp, obuf, sizeof(obuf)))
		return -1;
	if (!u_read_cstr(uc, newp, nbuf, sizeof(nbuf)))
		return -1;
	return rename(obuf, nbuf);
}

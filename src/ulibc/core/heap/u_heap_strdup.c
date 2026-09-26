#include "../../include/u_heap.h"

#include "../../include/u_mem.h"

/**
 * @file u_heap_strdup.c
 * @brief u_heap_strdup —— 把客户机 C 串复制到新分配的客户机内存
 */

uint32_t u_heap_strdup(uc_engine *uc, uint32_t s) {
	uint32_t len = u_strlen(uc, s);
	uint32_t p = u_malloc(uc, len + 1);
	if (!p)
		return 0;
	u_memcpy(uc, p, s, len);
	u_wr8(uc, p + len, 0);
	return p;
}

#include "../../include/u_heap.h"

#include "../../include/u_mem.h"

/**
 * @file u_calloc.c
 * @brief u_calloc —— 分配并清零（依赖 u_malloc）
 */

uint32_t u_calloc(uc_engine *uc, uint32_t n, uint32_t size) {
	if (n == 0 || size == 0)
		return 0;
	/* 防整数溢出 */
	if (n > 0xFFFFFFFFu / size)
		return 0;

	uint32_t total = n * size;
	uint32_t p = u_malloc(uc, total);
	if (!p)
		return 0;
	u_memset(uc, p, 0, total);
	return p;
}

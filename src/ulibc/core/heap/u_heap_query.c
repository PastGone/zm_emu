#include "../../include/u_heap.h"

#include "../../include/internal/u_heap_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_heap_query.c
 * @brief u_heap_ready / u_heap_used / u_heap_free_bytes / u_heap_size
 *
 * 四个诊断查询同族且 free_bytes 由 used 派生，合并为一个翻译单元，
 * 避免把堆状态遍历逻辑复制多份。
 */

int u_heap_ready(uc_engine *uc) {
	return (u_heap_g.ready && u_heap_g.uc == uc) ? 1 : 0;
}

uint32_t u_heap_used(uc_engine *uc) {
	if (!u_heap_ready(uc))
		return 0;
	uint32_t used = 0;
	uint32_t cur = u_heap_g.base;
	while (cur + U_HEAP_HDR <= u_heap_g.end) {
		uint32_t flags = u_rd32(uc, cur);
		uint32_t bsz = flags & ~7u;
		if (bsz < U_BLK_MIN || cur + bsz > u_heap_g.end)
			break;
		if (flags & U_FLAG_USED)
			used += bsz;
		cur += bsz;
	}
	return used;
}

uint32_t u_heap_free_bytes(uc_engine *uc) {
	if (!u_heap_ready(uc))
		return 0;
	return u_heap_g.size - u_heap_used(uc);
}

uint32_t u_heap_size(uc_engine *uc) {
	if (!u_heap_ready(uc))
		return 0;
	return u_heap_g.size;
}

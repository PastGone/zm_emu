#include "../../include/u_heap.h"

#include "../../include/internal/u_heap_impl.h"

/**
 * @file u_heap_ensure.c
 * @brief u_heap_ensure —— 未初始化时才初始化，已初始化则空操作
 */

void u_heap_ensure(uc_engine *uc, uint32_t base, uint32_t size) {
	if (u_heap_g.ready && u_heap_g.uc == uc)
		return;
	u_heap_setup(uc, base, size);
}

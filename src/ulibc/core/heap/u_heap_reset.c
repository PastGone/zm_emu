#include "../../include/u_heap.h"

#include "../../include/internal/u_heap_impl.h"

/**
 * @file u_heap_reset.c
 * @brief u_heap_reset —— 复位堆（等价重新初始化），用于 applet 重启
 */

void u_heap_reset(uc_engine *uc) {
	if (!u_heap_g.ready || u_heap_g.uc != uc)
		return;
	u_heap_setup(uc, u_heap_g.base, u_heap_g.size);
}

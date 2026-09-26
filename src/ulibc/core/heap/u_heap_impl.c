
#include "../../include/internal/u_heap_impl.h"
#include "../../../log/log.h"
#include "../../include/u_mem.h"

/**
 * @file u_heap_impl.c
 * @brief 堆模块内部共享实现（非公开函数，勿直接调用）
 */

u_heap_state u_heap_g;

uint32_t u_heap_align8(uint32_t v) {
	return (v + 7u) & ~7u;
}

uint32_t u_blk_total(uc_engine *uc, uint32_t hdr) {
	return u_rd32(uc, hdr) & ~7u;
}

int u_blk_used(uc_engine *uc, uint32_t hdr) {
	return (u_rd32(uc, hdr) & U_FLAG_USED) != 0;
}

void u_blk_set_size(uc_engine *uc, uint32_t hdr, uint32_t total, int used) {
	u_wr32(uc, hdr, (total & ~7u) | (used ? U_FLAG_USED : 0u));
}

void u_blk_set_prev(uc_engine *uc, uint32_t hdr, uint32_t prev_total) {
	u_wr32(uc, hdr + 4, prev_total);
}

void u_heap_setup(uc_engine *uc, uint32_t base, uint32_t size) {
	u_heap_g.uc = uc;
	u_heap_g.base = base;
	u_heap_g.size = size & ~7u;
	u_heap_g.end = base + u_heap_g.size;
	u_heap_g.ready = 0;

	if (!uc || u_heap_g.size < U_BLK_MIN) {
		log_error("u_heap: 初始化失败 base=0x%08X size=%u", base, size);
		return;
	}
	u_blk_set_size(uc, base, u_heap_g.size, 0);
	u_blk_set_prev(uc, base, 0);
	u_heap_g.ready = 1;
}

#include "../../include/u_heap.h"

#include "../../include/internal/u_heap_impl.h"

/**
 * @file u_heap_init.c
 * @brief u_heap_init —— 初始化客户机堆（丢弃此前所有分配）
 */

void u_heap_init(uc_engine *uc, uint32_t base, uint32_t size) {
  u_heap_setup(uc, base, size);
}

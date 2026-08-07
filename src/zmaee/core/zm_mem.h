#ifndef ZM_MEM_H
#define ZM_MEM_H

#include <stdint.h>

/* 客户机堆分配：bump + 空闲链表复用，耗尽时返回 0 并打错误日志 */
uint32_t host_malloc(uint32_t *heap_ptr, uint32_t size);

/* 归还一块由 host_malloc 分配的内存（非本分配器的地址会被安全忽略） */
void host_free(uint32_t ptr);

/* 已使用 / 峰值使用的堆字节数，用于运行摘要 */
uint32_t host_heap_used(uint32_t heap_ptr);
uint32_t host_heap_peak(void);

#endif

#ifndef U_HEAP_IMPL_H
#define U_HEAP_IMPL_H
/**
 * @file u_heap_impl.h
 * @brief 堆模块的内部共享状态与块头操作
 *
 * musl 风格下 malloc/free/calloc/realloc/heap_query 各是独立翻译单元，
 * 但必须共享同一份堆状态与块头读写逻辑，故集中于此（对应 musl 的
 * src/internal/）。这些符号是内部实现细节，外部不应调用。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

#include "../u_heap.h"

#define U_FLAG_USED 1u
/** 最小块 = 头 8 + 载荷至少 8 */
#define U_BLK_MIN (U_HEAP_HDR + U_HEAP_ALIGN)

typedef struct {
  uc_engine *uc;
  uint32_t base;
  uint32_t size; /* 8 字节对齐后的可用总大小 */
  uint32_t end;
  int ready;
} u_heap_state;

/** 全局堆状态（定义于 core/heap/u_heap_impl.c） */
extern u_heap_state u_heap_g;

/* 块头操作：total = size_flags & ~7，used = size_flags & 1 */
uint32_t u_blk_total(uc_engine *uc, uint32_t hdr);
int u_blk_used(uc_engine *uc, uint32_t hdr);
void u_blk_set_size(uc_engine *uc, uint32_t hdr, uint32_t total, int used);
void u_blk_set_prev(uc_engine *uc, uint32_t hdr, uint32_t prev_total);

/** 8 字节对齐 */
uint32_t u_heap_align8(uint32_t v);

/** 重建整块区为单一空闲块；u_heap_init/ensure/reset 共用 */
void u_heap_setup(uc_engine *uc, uint32_t base, uint32_t size);

#endif /* U_HEAP_IMPL_H */

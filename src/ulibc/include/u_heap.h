#ifndef U_HEAP_H
#define U_HEAP_H
/**
 * @file u_heap.h
 * @brief 客户机（Unicorn 模拟内存）堆分配器 —— 一级基石之一
 *
 * 为什么必须重写而不能透传宿主 malloc：
 *   宿主 malloc 返回的是**宿主真实地址**，客户机拿着它去访问会落到
 *   Unicorn 的未映射区间，直接触发 UC_HOOK_MEM_UNMAPPED 甚至崩溃。
 *   因此堆必须建在客户机已映射区间内，返回**客户机地址**。
 *
 * 实现：带边界标记的隐式空闲链表（first-fit + 相邻空闲块合并）。
 *
 * 客户机内存布局（每块）：
 *   +0  size_flags : 块总大小（含 8 字节头），bit0 = 已用标志
 *   +4  prev_size  : 物理前驱块总大小（0 = 本块是第一块），用于向前合并
 *   +8  payload    : 返回给调用者的地址（8 字节对齐）
 *
 * 其中 u_malloc / u_free / u_calloc / u_realloc 是**标准 C** 函数；
 * 其余（u_heap_*）是模拟器基建，标准 C 里没有。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

/** 块头大小（字节） */
#define U_HEAP_HDR 8u
/** 对齐粒度 */
#define U_HEAP_ALIGN 8u

/* -------------------- 基建：堆初始化与诊断 -------------------- */

/**
 * 初始化客户机堆。base/size 必须是已经 uc_mem_map 过的区间。
 * 会复位整个堆（丢弃之前所有分配）。
 */
void u_heap_init(uc_engine *uc, uint32_t base, uint32_t size);

/** 若尚未初始化则按 base/size 初始化；已初始化则不做任何事 */
void u_heap_ensure(uc_engine *uc, uint32_t base, uint32_t size);

/** 复位堆（等价于重新 u_heap_init），用于 applet 重启 */
void u_heap_reset(uc_engine *uc);

/** 堆是否已就绪（未就绪时所有分配返回 0） */
int u_heap_ready(uc_engine *uc);

/** 已用字节数 / 空闲字节数 / 总字节数（诊断用） */
uint32_t u_heap_used(uc_engine *uc);
uint32_t u_heap_free_bytes(uc_engine *uc);
uint32_t u_heap_size(uc_engine *uc);

/* -------------------- 标准 C：stdlib.h 分配函数 -------------------- */

/** 分配 size 字节，返回客户机地址；失败返回 0 */
uint32_t u_malloc(uc_engine *uc, uint32_t size);

/** 释放；p 为 0 或非法地址时安全返回 */
void u_free(uc_engine *uc, uint32_t p);

/** 分配 n*size 字节并清零；整数溢出时返回 0 */
uint32_t u_calloc(uc_engine *uc, uint32_t n, uint32_t size);

/** 重分配：优先原地扩张（合并后继空闲块），否则 malloc + memcpy + free */
uint32_t u_realloc(uc_engine *uc, uint32_t p, uint32_t size);

/* -------------------- 扩展：堆上字符串复制 -------------------- */

/** 把客户机 C 串复制到新分配的客户机内存中（POSIX strdup 语义，返回客户机地址） */
uint32_t u_heap_strdup(uc_engine *uc, uint32_t s);

#endif /* U_HEAP_H */

#ifndef HOOK_H
#define HOOK_H

#include "./emu.h"
// -------------------- Unicorn 钩子回调函数 --------------------

/* UC_HOOK_CODE 回调：反汇编 + trap 检测 + 暂停 */
void hook_code(uc_engine *uc, uint64_t address, uint32_t size, void *user_data);

/* UC_HOOK_MEM_READ/WRITE 回调：shim 区读写日志 */
void hook_shim_mem(uc_engine *uc, uc_mem_type type, uint64_t address, int size,
                   int64_t value, void *user_data);

/* UC_HOOK_MEM_UNMAPPED 回调：未映射内存访问警告 */
bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address,
                       int size, int64_t value, void *user_data);

/* PC 观察点（纯调试）。env: ZM_PC=<起址>[,<止址>]（十六进制，可带 0x）
 * 命中区间时打印 PC/LR/R0-R3，用来确认"某个函数被调用时拿到什么参数"。
 * 不设置时完全不生效（每指令只多一次 int 判断），不改变任何模拟行为。 */
void hook_set_pc_watch(uint32_t lo, uint32_t hi);

/* 同上，第二个/第 N 个观察点（ZM_PC 用 idx=0，ZM_PC2 用 idx=1）。 */
void hook_set_pc_watch_idx(int idx, uint32_t lo, uint32_t hi);

/* 内存写监视（纯调试）。env: ZM_MW=<起址>,<止址>（十六进制）
 * 命中区间被写入时打印 PC/LR/地址/值，用来查"某个字段到底有没有人写、
 * 是谁写的"。不设置时完全不生效，不改变任何模拟行为。 */
bool hook_mem_write_watch(uc_engine *uc, uc_mem_type type, uint64_t address,
                          int size, int64_t value, void *user_data);

#endif
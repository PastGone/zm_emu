#ifndef HOOK_H
#define HOOK_H

#include "./emu.h"
// -------------------- Unicorn 钩子回调函数 --------------------

/* UC_HOOK_CODE 回调：反汇编 + trap 检测 + 分发 */
void hook_code(uc_engine *uc, uint64_t address, uint32_t size, void *user_data);

/* UC_HOOK_MEM_READ/WRITE 回调：shim 区读写日志 */
void hook_shim_mem(uc_engine *uc, uc_mem_type type, uint64_t address, int size,
                   int64_t value, void *user_data);

/* UC_HOOK_MEM_UNMAPPED / UC_HOOK_MEM_PROT 回调：
 * 按需补映射并返回 true，让模拟继续跑而不是异常退出 */
bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address,
                       int size, int64_t value, void *user_data);

/* UC_HOOK_INSN_INVALID 回调：把非法指令当作 4 字节 NOP 跳过 */
bool hook_insn_invalid(uc_engine *uc, void *user_data);

/* UC_HOOK_CODE 回调（仅零页）：空函数指针调用（blx 0）按"返回 0"跳过 */
void hook_null_call(uc_engine *uc, uint64_t address, uint32_t size,
                    void *user_data);

#endif

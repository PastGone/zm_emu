#ifndef __HOOK_H__
#define __HOOK_H__

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

#endif
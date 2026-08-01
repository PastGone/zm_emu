#ifndef __TRAP_H__
#define __TRAP_H__

#include "./emu.h"

// -------------------- trap 调度器 --------------------

/**
 * @brief 陷阱调度器：根据 trap 地址分发到对应的 zm_* 处理函数
 *
 * 由 hook_code 在发现 PC 位于 TRAMP 区时调用。
 *
 * @param uc           Unicorn 引擎
 * @param trap_address 当前 PC 地址（应在 TRAMP_BASE ~ TRAMP_BASE+TRAMP_SIZE
 * 范围内）
 * @param r0..r3       当前寄存器值
 * @param sp, lr       当前 SP / LR 值
 */
void handle_trap(uc_engine *uc, uint32_t trap_address);

#endif
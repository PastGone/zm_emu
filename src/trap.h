#ifndef TRAP_H
#define TRAP_H

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
 */
void handle_trap(uc_engine *uc, uint32_t trap_address);

/**
 * @brief 读取第 n 个客户机调用参数（n=0.. 对应 r0/r1/r2/r3/栈）
 *
 * 前提：当前 PC 必须停在被调函数的第一条指令（trap 入口即如此），
 * 此时 r0..r3 是前 4 个参数，第 5 个起在 SP + (n-4)*4。
 * 供 display/image 等需要 5 个以上参数的槽使用。
 */
uint32_t getArg(uc_engine *uc, uint32_t n);

/**
 * @brief 打印"最近 32 次 applet→外部（槽）调用"（崩溃时调，见 emu.c）
 *
 * 目的：回答"崩之前它刚调了哪些槽、入参是什么" —— 排查"某槽返回值被当指针/
 * 尺寸用"引起的野跳、野读。ZM_NO_TRAP_RING=1 可关掉记录。
 */
void trap_dump_recent(void);

#endif
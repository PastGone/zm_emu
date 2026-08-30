#include "../../include/u_jmp.h"

/**
 * @file u_longjmp.c
 * @brief u_longjmp —— 恢复上下文并跳转（依赖 u_ctx_restore）
 *
 * 返回方式：本模拟器里 libc 通过 trap 进入，trap 出口会执行 PC = LR。
 * 因此这里只需恢复 LR 为 setjmp 记录的返回地址、R0 设为 val，
 * 跳转由 trap 出口自然完成，无需强行 uc_emu_stop / uc_context_restore。
 */

void u_longjmp(uc_engine *uc, uint32_t jmpbuf, int val) {
  if (!u_ctx_restore(uc, jmpbuf)) {
    /* 恢复失败：至少让返回值非 0，避免调用方死循环 */
    uint64_t r0 = 1;
    uc_reg_write(uc, UC_ARM_REG_R0, &r0);
    return;
  }
  uint64_t r0 = (val != 0) ? (uint64_t)(uint32_t)val : 1u;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
}

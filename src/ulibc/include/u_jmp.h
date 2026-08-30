#ifndef U_JMP_H
#define U_JMP_H
/**
 * @file u_jmp.h
 * @brief setjmp / longjmp —— 标准 C（setjmp.h）【必须重写·控制流类】
 *
 * 为什么不能透传宿主 setjmp：宿主保存的是**宿主的 CPU 寄存器**，
 * 客户机需要保存/恢复的是 Unicorn 的 ARM 寄存器，两者毫无关系。
 *
 * 客户机 jmp_buf 布局（11 × uint32 = 44 字节）：
 *   +0..+31  r4 .. r11   （AAPCS 要求被调方保存的寄存器）
 *   +32      sp
 *   +36      lr          （setjmp 的返回地址）
 *   +40      魔数 U_JMP_MAGIC
 *
 * 返回方式：本模拟器里 libc 是通过 trap 进入的，trap 处理器的统一出口会
 * 执行 `PC = LR`。因此 u_longjmp 只需把 LR 恢复成 setjmp 记录的返回地址，
 * 并把 R0 设为 val，随后由 trap 出口的 `PC = LR` 自然完成跳转，
 * 无需在回调里强行 uc_emu_stop / uc_context_restore。
 *
 * 若你的 trap 出口不写 PC=LR，请自行在调用 u_longjmp 后写 PC。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

#define U_JMPBUF_SIZE 44u
#define U_JMP_MAGIC 0x4A4D5042u /* "JMPB" */

/** 保存上下文，固定返回 0（与标准 setjmp 首次返回语义一致） */
int u_setjmp(uc_engine *uc, uint32_t jmpbuf);

/** 恢复上下文；val 为 0 时按标准改为 1 */
void u_longjmp(uc_engine *uc, uint32_t jmpbuf, int val);

/* -------------------- 基建：上下文存取（非标准） -------------------- */

/** 把当前上下文写入客户机 jmpbuf（u_setjmp 的底层实现，返回成功与否） */
int u_ctx_save(uc_engine *uc, uint32_t jmpbuf);

/** 从客户机 jmpbuf 恢复上下文（不含 R0），返回成功与否 */
int u_ctx_restore(uc_engine *uc, uint32_t jmpbuf);

#endif /* U_JMP_H */

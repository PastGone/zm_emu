#ifndef U_ARG_H
#define U_ARG_H
/**
 * @file u_arg.h
 * @brief AAPCS 变参游标 —— 解决「固定参数 / 不定参数」跨地址空间取参问题
 *
 * 【基建·非标准】整个文件都是模拟器专用机制，标准 C 里没有对应物。
 *
 * 背景（这是模拟器 libc 最容易踩的坑）：
 *   宿主 libc 的 va_list 是按**宿主 ABI**（x86-64 System V：寄存器保存区 +
 *   溢出区；或是 x86-64 的 gp_offset/fp_offset/overflow_arg_area 三元组）
 *   布局的结构体；客户机是 ARM32 AAPCS，两者字节布局完全不同，
 *   **绝对不能把客户机的 va_list 原样传给宿主 printf/vsnprintf**。
 *
 * 正确做法：在宿主侧自己维护一个「参数游标 u_va」，它只知道三件事：
 *   1. 参数从哪里来（寄存器 / 客户机内存 / 宿主数组）
 *   2. 已经取到第几个 4 字节槽
 *   3. 64 位参数需要 8 字节对齐（AAPCS 硬性要求）
 * 然后由 u_fmt 逐个 `u_va_u32() / u_va_i32() / u_va_f64()` 取值，
 * 再交给宿主做**单个**转换的格式化（snprintf("%08x", v)），
 * 从而彻底绕开 va_list 的 ABI 差异。
 *
 * 三种取参来源：
 *
 *   U_VA_REGS  —— 直接调用（客户机 BL 到 trap 地址，尚未执行函数序言）
 *                  第 0..3 个参数在 r0..r3，第 4 个起在入口 SP + (i-4)*4。
 *                  与 trap.c 里的 getArg() 完全同构。
 *                  u_va_start_regs(va, uc, n_named)：n_named 是**固定参数**
 *                  个数，例如 sprintf(dst, fmt, ...) 的 n_named = 2。
 *
 *   U_VA_MEM   —— 参数已连续存放在客户机内存（客户机函数把 r0-r3 溢出到
 *                  栈帧后转发 va_list，或 trap 处理器已经把参数打包）。
 *                  u_va_start_mem(va, uc, addr, n_named)
 *                  u_va_start_ap() / u_va_start_va()：直接吃客户机 va_list
 *                  （ARM 上 va_list 就是一个指向下一个参数的指针）。
 *
 *   U_VA_MEM8  —— 【SDK 的 sprintf 转发格式，实测 31 个 applet 全部一致】
 *                  参数区由 applet 自带的"溢出变参"助手构造：
 *                      +0   : 变参个数（dword）
 *                      +4   : 第 1 个变参（4 字节值）
 *                      +12  : 第 2 个变参
 *                      ...    即每个槽 8 字节，值放在槽首
 *                  整型/指针取 4 字节；double 由助手写 8 字节（会跨到下一槽
 *                  起始处），因此 64 位值需要连读 8 字节。
 *                  applet 的 sprintf 包装（如 00000506 sub_18EDC /
 *                  00001b62 同型代码）把该区地址放在 r2 后调用 ROOT[0x6C]。
 *
 *   U_VA_ARRAY —— 参数已在宿主侧 uint32_t 数组里（老 trap 处理函数常用），
 *                  零拷贝，最省事。
 *
 * 关于「所有变参都在栈上」：
 *   AAPCS 基础标准对变参函数的调用约定与普通函数**相同**（前 4 个进 r0-r3），
 *   只是被调方在序言里把它们溢出到栈以构造 va_list。所以：
 *     · 直接 trap 到 libc 函数 → 用 U_VA_REGS
 *     · 客户机转发 va_list 给 libc 的 v 系列 → 用 U_VA_MEM / U_VA_VA
 *   两种都支持，按调用点选用即可。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

enum { U_VA_REGS = 0, U_VA_MEM = 1, U_VA_ARRAY = 2, U_VA_MEM8 = 3 };

typedef struct {
  uc_engine *uc;
  int mode;
  uint32_t regs[4];  /* U_VA_REGS：入口 r0..r3 快照 */
  uint32_t sp;       /* U_VA_REGS：入口 SP（栈参数基址） */
  uint32_t mem_base; /* U_VA_MEM：参数区基址 */
  const uint32_t *arr;
  uint32_t arr_n;
  uint32_t next; /* 下一个参数的「槽序号」 */
} u_va;

/** 从入口寄存器 + 栈取参；n_named = 固定参数个数 */
void u_va_start_regs(u_va *va, uc_engine *uc, uint32_t n_named);

/** 所有参数连续存放在客户机 addr；第 i 个参数在 addr + i*4 */
void u_va_start_mem(u_va *va, uc_engine *uc, uint32_t addr, uint32_t n_named);

/**
 * SDK 的 sprintf 变参区（见文件头 U_VA_MEM8 说明）：
 *   addr + 0    : 变参个数
 *   addr + 4 + i*8 : 第 i 个变参（4 字节值；double 为 8 字节）
 */
void u_va_start_mem8(u_va *va, uc_engine *uc, uint32_t addr, uint32_t n_named);

/** 所有参数在宿主数组里 */
void u_va_start_array(u_va *va, const uint32_t *args, uint32_t nargs);

/** 客户机 va_list.__ap（指向下一个变参的指针）*/
void u_va_start_ap(u_va *va, uc_engine *uc, uint32_t ap);

/** 客户机 &va_list 的地址：读出 __ap 后按 mem 模式取参，返回 0 成功 / -1 失败 */
int u_va_start_va(u_va *va, uc_engine *uc, uint32_t va_list_addr);

/** 回退 n 个槽（重复取同一个参数时用） */
void u_va_rewind(u_va *va, uint32_t n);

/** 跳过 n 个槽 */
void u_va_skip(u_va *va, uint32_t n);

/** 当前游标在客户机中的地址（U_VA_REGS 且还在寄存器里时为 0） */
uint32_t u_va_ptr(u_va *va);

uint32_t u_va_u32(u_va *va);
int32_t u_va_i32(u_va *va);
uint64_t u_va_u64(u_va *va);
int64_t u_va_i64(u_va *va);
double u_va_f64(u_va *va);
float u_va_f32(u_va *va);

/** 仅为与标准 va_end 对仗，当前实现为空操作 */
void u_va_end(u_va *va);

#endif /* U_ARG_H */

/**
 * @file trap_debug.c
 * @brief trap 的调试基础设施：参数读取、寄存器 dump、调用环形缓冲、回调探针
 *
 * 全部是"观察"性质的东西，不参与分派。对外（trap_dispatch.c）只暴露四个口子：
 *   trap_debug_before() —— 进 handler 前（参数/寄存器 dump + 探针 + 上下文镜像）
 *   trap_ring_record()  —— 记一笔调用，拿到下标
 *   trap_debug_after()  —— 回填返回值
 *   trap_debug_probe()  —— ZM_CB_PROBE
 * 这样 handle_trap 主体只剩"读寄存器 → 查表 → 调 fn → 回写 R0/PC"。
 */

#include <stdio.h>
#include <stdlib.h> /* getenv */

#include "../debug/zm_stat.h" /* zm_stat_trap：槽位调用次数统计（ZM_STAT=1） */
#include "../hook.h"          /* hook_ctx_apply：applet 上下文镜像 */
#include "../log/log.h"
#include "trap_internal.h"

/* -------------------- 参数读取 -------------------- */

uint32_t getArg(uc_engine *uc, uint32_t n) {
  uint64_t v64 = 0; // 关键：必须用 64 位容器
  uc_err err;

  // 1. 寄存器参数 R0 ~ R3
  if (n <= 3) {
    err = uc_reg_read(uc, UC_ARM_REG_R0 + n, &v64);
    if (err != UC_ERR_OK)
      return 0;
    return (uint32_t)v64;
  }

  // 2. 栈参数（前提：当前 PC 必须在函数第一条指令！）
  uint64_t sp = 0;
  err = uc_reg_read(uc, UC_ARM_REG_SP, &sp);
  if (err != UC_ERR_OK)
    return 0;
  if (sp == STACK_TOP) {
    log_info("sp is STACK_TOP");
    return 0;
  }
  // 第 n 个参数（n>=4）在入口栈顶的偏移 (n-4)*4
  uint64_t addr = sp + (n - 4) * 4;
  uint32_t result = 0;
  err = uc_mem_read(uc, addr, &result, 4);
  if (err != UC_ERR_OK)
    return 0;

  return result;
}

/* -------------------- 寄存器 dump -------------------- */

/**
 * 打印 Unicorn 模拟器中当前所有非零的 ARM32 核心寄存器
 * @param uc Unicorn 引擎句柄
 */
static void print_non_zero_registers(uc_engine *uc) {
  // 定义需要遍历的寄存器结构
  typedef struct {
    int reg_id;       // Unicorn 定义的寄存器 ID (UC_ARM_REG_*)
    const char *name; // 寄存器名称（用于打印）
  } arm_reg_entry;

  // 针对 MT6250 (ARM7EJ-S) 的核心寄存器列表
  // 去掉了浮点/NEON/Cortex-M专有寄存器，因为MT6250通常不支持或不存在
  arm_reg_entry regs[] = {// 通用寄存器 R0 - R12
                          {UC_ARM_REG_R0, "R0"},
                          {UC_ARM_REG_R1, "R1"},
                          {UC_ARM_REG_R2, "R2"},
                          {UC_ARM_REG_R3, "R3"},
                          {UC_ARM_REG_R4, "R4"},
                          {UC_ARM_REG_R5, "R5"},
                          {UC_ARM_REG_R6, "R6"},
                          {UC_ARM_REG_R7, "R7"},
                          {UC_ARM_REG_R8, "R8"},
                          {UC_ARM_REG_R9, "R9"},
                          {UC_ARM_REG_R10, "R10"},
                          {UC_ARM_REG_R11, "R11"},
                          {UC_ARM_REG_R12, "R12"},
                          // 栈指针、链接寄存器、程序计数器
                          {UC_ARM_REG_SP, "SP"}, // R13
                          {UC_ARM_REG_LR, "LR"}, // R14
                          {UC_ARM_REG_PC, "PC"}, // R15
                                                 // 状态寄存器
                          {UC_ARM_REG_CPSR, "CPSR"},
                          {UC_ARM_REG_SPSR, "SPSR"}};

  int reg_count = sizeof(regs) / sizeof(regs[0]);
  uint32_t value = 0;
  uc_err err = UC_ERR_OK;
  int has_non_zero = 0; // 标记是否有非零寄存器

  printf("========== Non-zero Registers ==========\n");

  for (int i = 0; i < reg_count; i++) {
    // 读取寄存器值
    err = uc_reg_read(uc, regs[i].reg_id, &value);

    if (err == UC_ERR_OK) {
      if (value != 0) {
        printf("[+] %s: 0x%08X (%u)\n", regs[i].name, value, value);
        has_non_zero = 1;
      }
    } else {
      // 读取失败（例如在用户模式下读取 SPSR 会报错），静默跳过即可
      // 如果需要调试，可以取消下面注释：
      // printf("[!] Read %s failed (error: %d)\n", regs[i].name, err);
    }
  }

  if (!has_non_zero) {
    printf("(No non-zero registers found in current context.)\n");
  }
  printf("========================================\n");
}

/* -------------------- 调用环形缓冲 -------------------- */

/* 最近若干次"applet → 外部（槽）"的调用记录（环形缓冲）。
 *
 * 崩溃时由 trap_dump_recent() 打出来，回答"崩之前它刚调了哪些槽、入参是什么" ——
 * 排查"某个返回值被当指针/尺寸用"导致的野跳、野读时，比翻上万行指令轨迹快得多
 * （实测 000004dc：崩在 0x128334DC 的取指，靠它才看清是哪一步喂了坏值）。
 * 只记 4 个入参 + 返回地址，开销可忽略；ZM_NO_TRAP_RING=1 可关。 */
#define TRAP_RING 32
static struct {
  uint32_t addr, r0, r1, r2, r3, lr, ret;
} s_ring[TRAP_RING];
static int s_ring_i = 0, s_ring_n = 0;

int trap_ring_record(uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2,
                     uint32_t r3, uint32_t lr) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *e = getenv("ZM_NO_TRAP_RING");
    disabled = (e && e[0] == '1') ? 1 : 0;
  }
  if (disabled)
    return -1;
  int slot = s_ring_i;
  s_ring[slot].addr = addr;
  s_ring[slot].r0 = r0;
  s_ring[slot].r1 = r1;
  s_ring[slot].r2 = r2;
  s_ring[slot].r3 = r3;
  s_ring[slot].lr = lr;
  s_ring[slot].ret = 0;
  s_ring_i = (s_ring_i + 1) % TRAP_RING;
  if (s_ring_n < TRAP_RING)
    s_ring_n++;
  /* 返回下标供调用方回填返回值（注意陷阱可能嵌套，必须用下标而不是"最后一条"） */
  return slot;
}

void trap_debug_after(int ring, uint32_t ret) {
  if (ring >= 0)
    s_ring[ring].ret = ret; /* 崩溃时能一眼看出"哪个槽回了什么" */
}

void trap_dump_recent(void) {
  if (s_ring_n <= 0)
    return;
  log_error("最近 %d 次外部调用（从旧到新；槽号是相对 TRAMP/CBK 窗口的偏移）：",
            s_ring_n);
  int start = (s_ring_i - s_ring_n + TRAP_RING) % TRAP_RING;
  for (int k = 0; k < s_ring_n; k++) {
    int i = (start + k) % TRAP_RING;
    uint32_t a = s_ring[i].addr;
    char tag[32];
    if (a >= TRAMP_BASE && a < TRAMP_BASE + TRAMP_SIZE)
      snprintf(tag, sizeof(tag), "槽+0x%X", a - TRAMP_BASE);
    else if (a >= CBK_TRAP_BASE && a < CBK_TRAP_BASE + CBK_TRAP_SIZE)
      snprintf(tag, sizeof(tag), "CBK文件+0x%X", a - CBK_TRAP_BASE);
    else
      snprintf(tag, sizeof(tag), "0x%X", a);
    log_error("  #%d %s r0=0x%X r1=0x%X r2=0x%X r3=0x%X lr=0x%X -> 返回 0x%X",
              k + 1, tag, s_ring[i].r0, s_ring[i].r1, s_ring[i].r2,
              s_ring[i].r3, s_ring[i].lr, s_ring[i].ret);
  }
}

/* -------------------- 进入 handler 前的观察 -------------------- */

void trap_debug_before(uc_engine *uc, uint32_t trap, uint32_t r0, uint32_t r1,
                       uint32_t r2, uint32_t r3, uint32_t lr) {
  (void)r0;
  (void)r1;
  (void)r2;
  (void)r3;
  (void)lr;
  /* 参数打印：仅在开启反汇编调试（ZM_DISASM/g_disasm）时输出。
   * 曾经是无条件 printf，每次 trap 都刷 10 行，既拖慢模拟又污染 stdout。 */
  if (g_disasm) {
    for (int i = 0; i < 10; i++) {
      uint32_t v = getArg(uc, i);
      log_debug("trap[0x%X] arg%d = 0x%08X", trap - TRAMP_BASE, i, v);
    }
  }

  /* 寄存器 dump 只在反汇编调试下输出（原来每次都刷，既慢又淹没有效日志） */
  if (g_disasm)
    print_non_zero_registers(uc);

  /* 统计探针（ZM_STAT=1）：在真正分发前记一笔，供"哪个槽位被疯狂调用"分析。
   * 只对常规 TRAMP 区记（CBK 跳板页那边减去 TRAMP_BASE 会得到个巨大的槽号 ✗） */
  if (trap >= TRAMP_BASE && trap < TRAMP_BASE + TRAMP_SIZE)
    zm_stat_trap(trap - TRAMP_BASE);

  /* applet 上下文镜像（见 hook.c）。它不是调试，但必须在 handler 之前跑，
   * 所以一并收在这里 —— handle_trap 主体因此不必知道它的存在。 */
  hook_ctx_apply(uc);
}

/* -------------------- ZM_CB_PROBE -------------------- */

/**
 * ZM_CB_PROBE=1：找 applet 的"**注册回调**"API
 *
 * 思路：如果某次外部调用的入参里出现"**像代码地址**"的值（落在 applet 的
 * payload 区 [0x100, 0x40000) 且 4 字节对齐），那多半是在把**函数指针**交给
 * 框架（注册回调 / 设处理器）。按 (槽, 第几个参数) 去重后打印，避免刷屏。
 *
 * 为什么需要：实测 000004dc《仙剑》的帧循环是回调驱动的 —— 主循环里
 *   `if (根表[0x10] != 0) return;`（有回调 ⇒ 由框架驱动 ⇒ 自己不干活）
 * 而我们没实现"注册/驱动回调"这块 ✗ ⇒ 界面永远不动 ✓。靠它能一眼找出
 * applet 到底把回调交给了哪个槽 ✓。
 */
void trap_debug_probe(uint32_t trap, uint32_t r0, uint32_t r1, uint32_t r2,
                      uint32_t r3, uint32_t lr) {
  if (!getenv("ZM_CB_PROBE"))
    return;
  static uint32_t seen[96][2];
  static int n_seen = 0;
  const uint32_t args[4] = {r0, r1, r2, r3};
  for (int k = 0; k < 4; k++) {
    uint32_t v = args[k];
    if (v < 0x100u || v >= 0x40000u || (v & 3u))
      continue;
    int dup = 0;
    for (int q = 0; q < n_seen; q++)
      if (seen[q][0] == trap && seen[q][1] == (uint32_t)k)
        dup = 1;
    if (dup)
      continue;
    if (n_seen < 96) {
      seen[n_seen][0] = trap;
      seen[n_seen][1] = (uint32_t)k;
      n_seen++;
    }
    if (trap >= TRAMP_BASE && trap < TRAMP_BASE + TRAMP_SIZE)
      log_info("回调探针: 槽+0x%X r%d=0x%X | r0=0x%X r1=0x%X r2=0x%X r3=0x%X lr=0x%X",
               trap - TRAMP_BASE, k, v, r0, r1, r2, r3, lr);
    else
      log_info("回调探针: 0x%X r%d=0x%X | r0=0x%X r1=0x%X r2=0x%X r3=0x%X lr=0x%X",
               trap, k, v, r0, r1, r2, r3, lr);
  }
}

#include "./hook.h"
#include "./log/log.h"
#include "./tool/disasm_log.h"
#include "./trap.h"
#include "./zmaee/gfx/zm_display.h" /* zm_display_pump_events：周期性泵窗口事件 */
#include "./zmaee/runtime/timer/zm_timer.h" /* zm_timer_interrupt：指令级定时器派发 */
#include <inttypes.h>
#include <stdio.h>

/* PC 观察点（ZM_PC / ZM_PC2，各一个区间，默认关闭）。只观察、不改变行为。 */
#define PC_WATCH_MAX 2
static int g_pc_watch_on = 0;
static uint32_t g_pc_watch_lo[PC_WATCH_MAX] = {0, 0};
static uint32_t g_pc_watch_hi[PC_WATCH_MAX] = {0, 0};
static int g_pc_watch_n[PC_WATCH_MAX] = {0, 0};

void hook_set_pc_watch_idx(int idx, uint32_t lo, uint32_t hi) {
  if (idx < 0 || idx >= PC_WATCH_MAX)
    return;
  g_pc_watch_lo[idx] = lo;
  g_pc_watch_hi[idx] = hi;
  g_pc_watch_on = 1;
  log_info("[PC观察%d] 命中区间 0x%X ~ 0x%X 时打印 PC/LR/R0-R3", idx + 1, lo,
           hi);
}

void hook_set_pc_watch(uint32_t lo, uint32_t hi) {
  hook_set_pc_watch_idx(0, lo, hi);
}

void hook_code(uc_engine *uc, uint64_t address, uint32_t size,
               void *user_data) {
  // 这个地方好像不对,因为啥来ARM32的规定，pc=address+8

  /* 周期性泵一次 SDL 窗口事件。
   * 背景：emu.c 用 uc_emu_start(..., 0, 0)（无限指令）驱动 guest，宿主只在
   * guest 调 IDisplay::Refresh 槽时才经 fb_commit 泵事件。一旦 guest 长时间
   * 自旋 / 卡在某个循环里不再调 Refresh，宿主就永远没机会处理窗口事件 ——
   * 表现为整个桌面"鼠标能动、点击没反应"、连窗口都关不掉。
   * 这里每 2^18 条指令泵一次（约几毫秒一次，开销可忽略），保证窗口始终可响应。 */
  {
    static uint32_t s_pump_tick = 0;
    if (((++s_pump_tick) & 0x3FFFFu) == 0)
      zm_display_pump_events();
  }

  /* 指令级定时器派发：到期的定时器直接打断当前执行。
   *
   * 背景：宿主原本只在 applet 回事件循环时（zm_display_event_loop）才
   * 检查定时器；applet 一旦在游戏/模态循环里自旋就再也不回事件循环，
   * 定时器全被饿死（实测 00000442：54 秒只回 3 次，1 秒定时器和 500ms
   * 心跳都不动 → 画面上"执行时间/倒数计时"永远不涨）。真机的这两类
   * 定时器由 Java 层回调触发，与 applet 自己的循环无关，所以这里补齐。
   * 每 2^16 条指令一次（比上面的 SDL 泵更密，保证 1 秒级定时器不漂移）。 */
  {
    static uint32_t s_timer_tick = 0;
    if (((++s_timer_tick) & 0xFFFFu) == 0) {
      if (zm_timer_interrupt(uc, (uint32_t)address))
        return; /* 已改写 PC/LR → 让 Unicorn 直接去跑回调 */
    }
  }

  if (g_pc_watch_on) {
    for (int i = 0; i < PC_WATCH_MAX; i++) {
      if (!g_pc_watch_hi[i] || address < g_pc_watch_lo[i] ||
          address > g_pc_watch_hi[i] || g_pc_watch_n[i] >= 400)
        continue;
      uint32_t r[8] = {0}, lr = 0;
      uc_reg_read(uc, UC_ARM_REG_LR, &lr);
      uc_reg_read(uc, UC_ARM_REG_R0, &r[0]);
      uc_reg_read(uc, UC_ARM_REG_R1, &r[1]);
      uc_reg_read(uc, UC_ARM_REG_R2, &r[2]);
      uc_reg_read(uc, UC_ARM_REG_R3, &r[3]);
      uc_reg_read(uc, UC_ARM_REG_R4, &r[4]);
      uc_reg_read(uc, UC_ARM_REG_R5, &r[5]);
      uc_reg_read(uc, UC_ARM_REG_R6, &r[6]);
      uc_reg_read(uc, UC_ARM_REG_R7, &r[7]);
      g_pc_watch_n[i]++;
      log_info("[PC观察%d] #%d PC=0x%" PRIx64 " LR=0x%X R0=0x%X R1=0x%X "
               "R2=0x%X R3=0x%X R4=0x%X R5=0x%X R6=0x%X R7=0x%X",
               i + 1, g_pc_watch_n[i], address, lr, r[0], r[1], r[2], r[3], r[4],
               r[5], r[6], r[7]);
    }
  }

  if (g_disasm) {
    disassemble_and_log(uc, address, size);
  }

  if (address >= TRAMP_BASE && address < TRAMP_BASE + TRAMP_SIZE) {
    handle_trap(uc, address);
  }
}

void hook_shim_mem(uc_engine *uc, uc_mem_type type, uint64_t address, int size,
                   int64_t value, void *user_data) {
  if (type == UC_MEM_READ) {
    log_debug("[HOOK] 从 0x%016lx 地址处读取大小为:%d的数据，值为:0x%016lx\n",
              address, size, value);

  } else if (type == UC_MEM_WRITE) {
    log_debug("[HOOK] 向 0x%016lx 地址处写入大小为:%d的数据，值为:0x%016lx\n",
              address, size, value);
  } else {
    log_debug("[HOOK] 其他内存操作 (type=%d)\n", type);
  }
}

bool hook_mem_write_watch(uc_engine *uc, uc_mem_type type, uint64_t address,
                          int size, int64_t value, void *user_data) {
  static int n = 0;
  const char *cap = getenv("ZM_MW_MAX");
  int lim = cap ? atoi(cap) : 300;
  if (n < lim) {
    uint32_t pc = 0, lr = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    n++;
    log_info("[MW] #%d 写 0x%" PRIx64 " (+%d) = 0x%" PRIx64 " PC=0x%X LR=0x%X",
             n, address, size, (uint64_t)value, pc, lr);
  }
  return true;
}

bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address,
                       int size, int64_t value, void *user_data) {
  /* 打印崩溃时上下文：PC（正执行指令）、LR（返回地址）、R0-R3（调用参数），
   * 用于判断是哪个 stub 返回 0 被当函数指针/对象解引用。 */
  uint32_t pc = 0, lr = 0, r0 = 0, r1 = 0, r2 = 0, r3 = 0;
  uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  uc_reg_read(uc, UC_ARM_REG_R1, &r1);
  uc_reg_read(uc, UC_ARM_REG_R2, &r2);
  uc_reg_read(uc, UC_ARM_REG_R3, &r3);
  /* 调试：dump applet 固定区 0x180 的 ROOT_TABLE_ADDR 指针与 ROOT_TABLE_ADDR 表前 16 字节，
   * 定位 sub_84410 读 [ROOT_TABLE_ADDR+8] 得到 0 的原因。 */
  uint32_t root_slot = 0, root0 = 0, root8 = 0, rootc = 0;
  uc_mem_read(uc, BLOB_BASE + ROOT_SLOT_OFF, &root_slot, 4);
  uc_mem_read(uc, root_slot + 0, &root0, 4);
  uc_mem_read(uc, root_slot + 8, &root8, 4);
  uc_mem_read(uc, root_slot + 0xC, &rootc, 4);
  log_warn("  !! MEM unmapped @0x%" PRIx64 " size=%d"
           "  PC=0x%X LR=0x%X R0=0x%X R1=0x%X R2=0x%X R3=0x%X"
           "  [0x180]=0x%X [ROOT_TABLE_ADDR]=0x%X [ROOT_TABLE_ADDR+8]=0x%X [ROOT_TABLE_ADDR+C]=0x%X\n",
           address, size, pc, lr, r0, r1, r2, r3, root_slot, root0, root8,
           rootc);
  return false;
}
/**
 * applet 上下文钩子（挂在 [HEAP_BASE+0x60, +4)）。
 *
 * 实测（00000502）：applet 把自己的上下文指针写进 [HEAP_BASE+0x60]
 * （内存观察：`写 0x1a0060 = 0xed005c`，LR=0x16710），而 applet 侧读的是
 * `[[CBK_OBJ+0x48]+0x60]`（0x16064 → 0x1AD00）。它**从不写** [CBK_OBJ+0x48]。
 *
 * 但它会先后写入多个值（最早的 `0x1A0098` 是它自建的分配器 ✗），而且真正
 * 上下文被指向的字段可能要稍后才填好 —— 所以这里**只记录候选**，
 * 由 trap 入口（hook_ctx_apply）在候选满足条件时再真正镜像过去。
 */
static uint32_t g_ctx_cand = 0;
uint32_t hook_ctx_candidate(void) { return g_ctx_cand; }

bool hook_ctx_mirror(uc_engine *uc, uc_mem_type type, uint64_t address, int size,
                     int64_t value, void *user_data) {
  (void)uc; (void)type; (void)address; (void)size; (void)user_data;
  uint32_t v = (uint32_t)value;
  /* 只认**落在 CBK 自管堆里**的候选：真上下文实测是 0xED005C（从 CBK 堆分配
   * 的对象 ✓），而 applet 早期写的 0x1A0098 是它**自己堆里**的自建分配器 ✗。
   * 用地址区间就能干净分开，不必猜字段内容。 */
  if (v < CBKHEAP_BASE || v >= CBKHEAP_BASE + CBKHEAP_SIZE)
    return true;
  g_ctx_cand = v;
  uint32_t cur = 0;
  if (uc_mem_read(uc, CBK_OBJ + 0x48, &cur, 4) == UC_ERR_OK && cur != v) {
    uc_mem_write(uc, CBK_OBJ + 0x48, &v, 4);
    uint32_t sub = 0;
    uc_mem_read(uc, v + 0x60u, &sub, 4);
    log_info("applet 上下文镜像：[HEAP_BASE+0x60]=0x%X（其 +0x60=0x%X）→ "
             "[CBK_OBJ+0x48]",
             v, sub);
  }
  return true;
}

/**
 * 由 trap 入口调用：候选的 +0x60 非 0（applet 正是读 [ctx+0x60]）时才镜像，
 * 且只在 [CBK_OBJ+0x48] 仍是我们的假对象 CBK_CTX 时动手。
 */
void hook_ctx_apply(uc_engine *uc) {
  (void)uc; /* 镜像已在 hook 内立即完成（见 hook_ctx_mirror） */
}

#include "./hook.h"
#include "./event.h" /* zm_event_async_poll：触摸事件的异步派发 */
#include "./log/log.h"
#include "./tool/disasm_log.h"
#include "./trap.h"
#include "./zmaee/gfx/zm_display.h" /* zm_display_pump_events：周期性泵窗口事件 */
#include "./zmaee/runtime/timer/zm_timer.h" /* zm_timer_interrupt / zm_timer_async_call */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h> /* getenv：PC 环开关 */

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

/* 最近执行过的指令地址（环形，见 hook_code 里的说明） */
#define PC_RING 24
static uint32_t s_pc_ring[PC_RING];
static uint8_t s_pc_mode[PC_RING]; /* 1 = Thumb */
static int s_pc_i = 0, s_pc_n = 0;
static uc_engine *s_pc_uc = NULL; /* 只为崩溃时回读字节用 */

void hook_dump_pc_ring(void) {
  if (s_pc_n <= 0)
    return;
  log_error("最近执行过的 %d 条指令（从旧到新；`T` = Thumb，无标记 = ARM；"
            "payload 文件偏移 ≈ 地址 + 0x18C）：",
            s_pc_n);
  int start = (s_pc_i - s_pc_n + PC_RING) % PC_RING;
  char line[256];
  int off = 0;
  for (int k = 0; k < s_pc_n; k++) {
    int i = (start + k) % PC_RING;
    off += snprintf(line + off, sizeof(line) - (size_t)off, " %05X%s",
                    s_pc_ring[i], s_pc_mode[i] ? "T" : "");
    if ((k + 1) % 8 == 0 || k + 1 == s_pc_n) {
      log_error("  %s", line);
      off = 0;
      line[0] = '\0';
    }
  }

  /* 再补最后 4 条的**真实字节**：反汇编要看客户机内存里到底是什么，而不是我们
   * 以为的文件偏移 —— 实测 000004dc 就是靠这一步才发现"我以为的指令"和实际执行的
   * 对不上（偏了一次，于是把 pop 看成了别的）✗。 */
  int shown = s_pc_n < 4 ? s_pc_n : 4;
  for (int k = 0; k < shown; k++) {
    int i = (start + s_pc_n - shown + k) % PC_RING;
    uint8_t b[8] = {0};
    char hex[3 * 8 + 1];
    int q = 0;
    if (s_pc_uc &&
        uc_mem_read(s_pc_uc, s_pc_ring[i], b, sizeof(b)) == UC_ERR_OK)
      for (unsigned j = 0; j < sizeof(b); j++)
        q += snprintf(hex + q, sizeof(hex) - (size_t)q, "%02X ", b[j]);
    else
      snprintf(hex, sizeof(hex), "<读不出>");
    log_error("  0x%05X%s 字节: %s", s_pc_ring[i], s_pc_mode[i] ? "T" : "", hex);
  }
}

void hook_set_pc_watch(uint32_t lo, uint32_t hi) {
  hook_set_pc_watch_idx(0, lo, hi);
}

void hook_code(uc_engine *uc, uint64_t address, uint32_t size,
               void *user_data) {
  // 这个地方好像不对,因为啥来ARM32的规定，pc=address+8

  /* ---- "最近执行过的地址"环形记录（崩溃诊断）----
   * 目的：崩在**非陷阱**的野地址（取指失败 / 野跳）时，日志最后一条往往是"某次
   * 陷阱调用"，看不到真正喂坏值的那条指令。这里每条指令前记一笔 PC（一次写内存，
   * 开销可忽略），崩溃时由 hook_dump_pc_ring() 打出 —— 通常一眼就能看出是
   * `blx r2`、`pop {…,pc}` 还是 `ldr pc,[rX]` 把 PC 带走的 ✓。
   * 实测 000004dc：崩在 0x128334DC（取指），靠它才定位到"从栈里弹回来的"那条路。
   * ZM_NO_PC_RING=1 可关。 */
  {
    static int on = -1;
    if (on < 0) {
      const char *e = getenv("ZM_NO_PC_RING");
      on = (e && e[0] == '1') ? 0 : 1;
    }
    if (on) {
      uint32_t cpsr = 0;
      s_pc_uc = uc;
      /* 顺带记 CPSR 的 T 位：崩在"野跳"时，先要能判断最后这几条是 ARM 还是
       * Thumb —— 同一串字节两种模式解出来完全不同，选错了就会看错指令 ✗
       *（实测 000004dc：ARM 看是 `pop {…,pc}`，其实那条路是 Thumb ✓）。 */
      if (uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr) != UC_ERR_OK)
        cpsr = 0;
      s_pc_ring[s_pc_i] = (uint32_t)address;
      s_pc_mode[s_pc_i] = (cpsr & 0x20) ? 1 : 0; /* 1 = Thumb */
      s_pc_i = (s_pc_i + 1) % PC_RING;
      if (s_pc_n < PC_RING)
        s_pc_n++;
    }
  }

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
    static uint32_t s_tick = 0, s_lo = 0xFFFFFFFFu, s_hi = 0;
    uint32_t pc = (uint32_t)address;
    if (pc < s_lo)
      s_lo = pc;
    if (pc > s_hi)
      s_hi = pc;
    if (((++s_tick) & 0xFFFFu) == 0) {
      /* ★ 只在该窗口内 **PC 跨度很小（真在自旋）** 时才打断。
       *
       * 为什么加这道闸：打断发生在**任意一条指令**处，而真机的定时器只在
       * 事件循环 / API 边界派发。实测 00000462《三国情仇》：点"开始游戏"后要跑
       * 几百毫秒的长初始化（PC 一路往前走），被我们插进去跑回调 → 状态被打断 →
       * 之后把没初始化的字段当指针用 → 崩在字体查表 / 精灵 blit（崩点每次不同 ✗）。
       * 而**真正需要**异步派发的自旋型 applet（0000048a：0ms 定时器驱动的主循环）
       * 是**在一个小圈里反复跳**，窗口内 PC 跨度极小 ✓ —— 用这个差别区分两类。
       * 阈值：ZM_ASYNC_SPREAD_KB（默认 8KB；0 = 关掉这道闸，退回旧行为）。 */
      /* ★ 区分"**瞬时长更新**"与"**真自旋**"：要求**连续多拍都处于饥饿**才打断。
       *
       * 为什么不能用"PC 跨度小"当判据（试过 ✗，方向正好相反）：
       *   0000048a（真自旋，需要派发）的窗口跨度**更大** ✗；而 00000462 出事的那段
       *   反而在**小圈**里（像是"等某个状态位"的紧循环）—— 被插进回调就会踩坏它。
       * 改用时间维度：00000462 的长更新只有约 300~400ms（一拍的量级）✗ ⇒ 不连饿；
       * 0000048a 是 32 秒不回事件循环 ✓ ⇒ 必然连饿 ⇒ 照常服务。
       * 需要连饿几拍：ZM_ASYNC_STARVE_TICKS（默认 2；1 = 退回旧行为）。 */
      static int need = -1;
      static uint32_t starved_run = 0;
      if (need < 0) {
        /* 默认 10：实测 00000462 连续饿到第 10 拍才说明它是"真自旋"（它的长更新
         * 会连饿 5 拍左右 ✗，被打破就崩）；而 0000048a 这种 32 秒不回事件循环的
         * 自旋型照样被服务（实测派发 123 次 ✓）。调小会误伤，调大则自旋型响应变慢。 */
        const char *e = getenv("ZM_ASYNC_STARVE_TICKS");
        need = (e && atoi(e) >= 1) ? atoi(e) : 10;
        log_info("异步派发：需连续 %d 拍处于饥饿（瞬时长更新不打断）", need);
      }
      s_lo = 0xFFFFFFFFu;
      s_hi = 0;
      if (zm_timer_is_starved(uc)) {
        if (starved_run < 0xFFFFu)
          starved_run++;
      } else {
        starved_run = 0;
      }
      if (starved_run >= (uint32_t)need) {
        if (zm_timer_interrupt(uc, pc))
          return; /* 已改写 PC/LR → 让 Unicorn 直接去跑回调 */
        if (zm_event_async_poll(uc, pc, true))
          return; /* 触摸：同样经跳板送进去 */
      }
    }
  }

  /* 触摸事件的异步派发已并入上面那个块（与定时器共用同一道"PC 跨度"闸）：
   * 背景是有些 applet 的主循环是"0ms 定时器驱动 + 自旋"，**再也不回事件循环**
   * （实测 0000048a：32 秒回 0 次），点击会被无限期饿死；现在只在"真自旋"的窗口里
   * 才用跳板送 evt=9/10 进去。ZM_NO_ASYNC_TOUCH=1 仍可单独关掉触摸这条。 */

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

  /* 陷阱两个区间：① 常规 TRAMP 区；② CBK 跳板页里的"文件对象"窗口
   * （CBK_TRAP_BASE，见 emu_mem_regions.h —— 这一族 applet 把
   * [CBK_OBJ+0x4C]→[+0] 的 +8 当"打开资源文件"的工厂用，那里必须是**我们能
   * 接手处理的入口**，而不是一段只会返回假对象的桩 ✗）。 */
  if ((address >= TRAMP_BASE && address < TRAMP_BASE + TRAMP_SIZE) ||
      (address >= CBK_TRAP_BASE && address < CBK_TRAP_BASE + CBK_TRAP_SIZE)) {
    handle_trap(uc, address);
  }
}

/* SHIM 区访问观察钩子（挂在 SHIM_BASE .. SHIM_BASE+SHIM_SIZE，注册见 emu.c）。
 *
 * 【为什么默认不再逐条打印】这片区域里放的是**全部 shim 对象的虚表与对象本体**，
 * applet 每做一次虚调用都要读它。一个"忙等"（自旋轮询）的 applet 每秒能在这里
 * 触发**两万多次**访问，而本函数原来对每次访问都打一行 log_debug —— 关键是不设
 * ZM_LOG 时默认等级曾是 LOG_TRACE（全开），于是"什么都不设"地跑一个自旋 applet
 * 就是刷屏：终端被几万行/秒的文本淹掉、模拟器被同步 I/O 拖住，**看上去整个卡死**
 * （实测某次：1 毫秒十几行、连续不断，窗口和画面都像不动了，实际还在跑）。
 *
 * 现在的策略——默认安静，要看时开开关：
 *   - 读：默认**完全不打印**；设 ZM_SHIM_TRACE=1 才打印，且默认最多 400 行
 *     （ZM_SHIM_MAX 可改，0 = 不限）。查"某处读 shim 拿到 0"这类问题时用。
 *   - 写：默认只打印前 64 次（ZM_SHIM_W_MAX 可改，0 = 不限）。写比读少几个数量级，
 *     而且"applet 覆写 shim 虚表/对象字段"是有价值的 RE 事件（例：CBK 对象 vt+8
 *     会被 applet 自己改写），所以保留但限流。
 *   - 到上限后各打一行汇总，然后彻底安静。
 * 环境变量只在首调用读一次，之后钩子内零 getenv 开销。 */
static int s_shim_cfg = -1; /* -1=未初始化 */
static int s_shim_r_trace = 0, s_shim_r_max = 400, s_shim_w_max = 64;
static unsigned s_shim_r_n = 0, s_shim_w_n = 0;
static int s_shim_r_done = 0, s_shim_w_done = 0;

static void shim_trace_init(void) {
  const char *t = getenv("ZM_SHIM_TRACE");
  const char *rm = getenv("ZM_SHIM_MAX");
  const char *wm = getenv("ZM_SHIM_W_MAX");
  s_shim_r_trace = (t && t[0] && t[0] != '0');
  s_shim_r_max = rm ? atoi(rm) : 400;
  s_shim_w_max = wm ? atoi(wm) : 64;
  s_shim_cfg = 1;
  if (s_shim_r_trace)
    log_info("[SHIM] SHIM 区读写观察已开：读上限 %d 行 / 写上限 %d 次（0=不限）",
             s_shim_r_max, s_shim_w_max);
}

void hook_shim_mem(uc_engine *uc, uc_mem_type type, uint64_t address, int size,
                   int64_t value, void *user_data) {
  (void)uc;
  (void)user_data;
  if (s_shim_cfg < 0)
    shim_trace_init();

  if (type == UC_MEM_READ) {
    if (!s_shim_r_trace)
      return;
    if (s_shim_r_max && s_shim_r_n >= (unsigned)s_shim_r_max) {
      if (!s_shim_r_done) {
        s_shim_r_done = 1;
        log_info("[SHIM] 读日志已达上限 %d 行，之后不再打印（调 ZM_SHIM_MAX，0=不限）",
                 s_shim_r_max);
      }
      return;
    }
    s_shim_r_n++;
    log_debug("[HOOK] 读 0x%X (%d 字节) = 0x%" PRIx64, (uint32_t)address, size,
              (uint64_t)value);
  } else if (type == UC_MEM_WRITE) {
    if (s_shim_w_max && s_shim_w_n >= (unsigned)s_shim_w_max) {
      if (!s_shim_w_done) {
        s_shim_w_done = 1;
        log_info("[SHIM] 写日志已达上限 %d 次，之后不再打印（调 ZM_SHIM_W_MAX，0=不限）",
                 s_shim_w_max);
      }
      return;
    }
    s_shim_w_n++;
    log_debug("[HOOK] 写 0x%X (%d 字节) = 0x%" PRIx64, (uint32_t)address, size,
              (uint64_t)value);
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

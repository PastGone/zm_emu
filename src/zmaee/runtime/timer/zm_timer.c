#include "zm_timer.h"

#include "../../../emu.h"
#include "../../../log/log.h"
#include "../../core/zm_root.h" /* zm_root_get_tick（SDL_GetTicks） */
#include <stdbool.h>
#include <stdint.h>
#include <unicorn/arm.h> /* UC_ARM_REG_*（定时器回调跳板） */

/* =========================================================================
 * ZMAEE IShell 定时器子系统（RE 全集）
 *
 * RE 依据（逐环节）：
 *   - ZMAEE_IShell_SetTimer   ：表结构 / ID 递增 / 表满 -1 / init 块
 *   - ZMAEE_IShell_CancelTimer：按 ID 查找 + 前移压缩 + 清尾槽，0/-1
 *   - ZMAEE_IShell_CancelOwnerTimer：按 entry[1]==owner 删，恒 0，
 *     删除后从下一槽继续扫（滑入条目被跳过——固件怪癖，已保真）
 *   - sub_34394（OS 滴答）：按表序取第一条过期 → 先 CancelTimer(id)
 *     （单次）→ cb(id, a5)；每 tick 至多一条；cb==0 仅移除（dladdr
 *     是 Android 壳层符号校验，客户机扁平内存下无意义，不复刻）
 *   - AndroidAEE_KillTimer：表排空收摊 + dword_65C64=0 → 下次
 *     SetTimer 重新 init、ID 归零
 * ========================================================================= */

#define ZM_TIMER_MAX 16 /* RE：count>15 → -1 */
typedef struct {
  uint32_t id;    /* entry[0]：SetTimer 返回的 ID */
  uint32_t owner; /* entry[1]：a4，RE 铁证（CancelOwnerTimer 匹配此字段） */
  uint32_t expire; /* entry[2]：到期时刻（GetTickCount 毫秒基准） */
  uint32_t a5;     /* entry[3]：回调第二参（RE 铁证：cb(r0=id, r1=a5)） */
  uint32_t cb;     /* entry[4]：a3，回调指针（RE 铁证：派发取此字段调用） */
} zm_timer_t;

static zm_timer_t s_timers[ZM_TIMER_MAX];
static uint32_t s_timer_count = 0;
static uint32_t s_timer_next_id = 0;
static bool s_tick_registered = false; /* 固件 dword_65C64 */

/* +0x3C SetTimer */
uint32_t zm_timer_SetTimer(uc_engine *uc, uint32_t dur_ms, uint32_t cb,
                           uint32_t owner, uint32_t a5) {
  /* RE：dword_65C64==0 时 init 块 memset 全表（含 ID 计数器）→
   * 首次注册或"排空后重新启用"时 ID 从 0 重新计数 */
  if (!s_tick_registered) {
    s_timer_next_id = 0;
    s_tick_registered = true;
  }
  if (s_timer_count > 15)
    return (uint32_t)-1; /* RE：count>15 → -1 */
  zm_timer_t *e = &s_timers[s_timer_count++];
  /* 【2026-09 实测修正】ID 从 1 开始，不用 0：
   *   00000001 的定时器回调 sub_68D8 会把 SetTimer 的返回值直接存进对象字段
   *   （0x6904: STR R0,[R4,#0x10]），之后该类的 getter（类表+0x20 → 0xD054
   *   的 LDR R0,[R0,#0x10]）把这个值当对象/句柄用。
   *   以前第一个 ID = 0 → 存进去就是 NULL → 被当 this → 崩在 pc=0x1EDC。
   *   真机上 ID 计数器早已被 AEE/壳层推进过，所以不会拿到 0；这里从 1 起步，
   *   语义等价（ID 只需唯一非 0）。 */
  e->id = ++s_timer_next_id;
  e->owner = owner;
  e->expire = zm_root_get_tick(uc) + dur_ms;
  e->a5 = a5;
  e->cb = cb;
  log_info("IShell.SetTimer(dur=%ums, cb=0x%08X, owner=0x%08X, a5=0x%08X) -> id=%u",
           dur_ms, cb, owner, a5, e->id);
  return e->id;
}

/* +0x40 CancelTimer：按 ID 查找 + 前移压缩（RE 同款） */
uint32_t zm_timer_CancelTimer(uc_engine *uc, uint32_t timer_id) {
  for (uint32_t i = 0; i < s_timer_count; ++i) {
    if (s_timers[i].id != timer_id)
      continue;
    for (; i + 1 < s_timer_count; ++i)
      s_timers[i] = s_timers[i + 1];
    s_timer_count--;
    s_timers[s_timer_count] = (zm_timer_t){0}; /* RE 清 vacated 尾槽 */
    log_info("IShell.CancelTimer(id=%u) -> 0", timer_id);
    return 0;
  }
  log_info("IShell.CancelTimer(id=%u) -> -1 (not found)", timer_id);
  return (uint32_t)-1;
}

/* +0x44 CancelOwnerTimer：删除全部 owner 匹配的定时器，恒返 0（RE）。
 * 保真复刻固件怪癖：删除后从下一槽继续扫——滑入当前槽的同 owner
 * 条目会被跳过、留在表里（与固件 decompile 逐语句一致）。 */
uint32_t zm_timer_CancelOwnerTimer(uc_engine *uc, uint32_t owner) {
  uint32_t idx = 0;
  while (idx < s_timer_count) {
    if (s_timers[idx].owner == owner) {
      zm_timer_CancelTimer(uc, s_timers[idx].id); /* RE：按 *(v4-1)=ID 删 */
      idx++; /* 固件怪癖：滑下来的条目不复查 */
    } else {
      idx++;
    }
  }
  return 0; /* RE：无论是否命中都返 0 */
}

/* =========================================================================
 * ROOT+0x148 / +0x14C：applet 级周期定时器（ZMAEE_Start_Timer/Stop_Timer）
 *
 * 固件结构（RE 反汇编 ZMAEE_Start_Timer @0x2E4E0、ZMAEE_Stop_Timer @0x2E498、
 * nativeAEETimerCallback @0x22E88）：一张 8 槽表，每槽 { id, cb, ... }，
 * 每次滴答遍历 8 槽、对已注册的槽调用 cb(r0=槽序号, r1=槽内参数)。
 * 这里按同样语义实现，只把"滴答"接到宿主事件循环（复用 zm_timer_poll 的
 * 派发路径，与 IShell 单次表互不影响）。
 * ========================================================================= */
#define ZM_RPT_MAX 8 /* RE：nativeAEETimerCallback 循环 8 次 */

typedef struct {
  bool used;
  uint32_t id;       /* Start_Timer 的 r1：applet 自己的唯一标识 */
  uint32_t cb;       /* r2：回调（客户机地址） */
  uint32_t interval; /* r0：周期毫秒 */
  uint32_t next;     /* 下次到期（宿主毫秒基准，同 zm_root_get_tick） */
} zm_rpt_timer_t;

static zm_rpt_timer_t s_rpt[ZM_RPT_MAX];

/* +0x14C Stop_Timer(id)：按 id 找槽后清空；未找到返 -1（RE 同款） */
uint32_t zm_timer_StopTimer(uc_engine *uc, uint32_t id) {
  (void)uc;
  for (uint32_t i = 0; i < ZM_RPT_MAX; ++i) {
    if (s_rpt[i].used && s_rpt[i].id == id) {
      s_rpt[i] = (zm_rpt_timer_t){0};
      log_info("ZMAEE_Stop_Timer(id=0x%08X) -> 0（槽 %u 释放）", id, i);
      return 0;
    }
  }
  log_info("ZMAEE_Stop_Timer(id=0x%08X) -> -1（未注册过）", id);
  return (uint32_t)-1;
}

/* +0x148 Start_Timer(interval_ms, id, cb)：先 Stop_Timer(id) 再占空槽（RE 同款） */
uint32_t zm_timer_StartTimer(uc_engine *uc, uint32_t interval_ms, uint32_t id,
                             uint32_t cb) {
  /* RE：固件进来第一件事就是 Stop_Timer(id) —— 同 id 重注册 = 先停旧的 */
  zm_timer_StopTimer(uc, id);

  int slot = -1;
  for (uint32_t i = 0; i < ZM_RPT_MAX; ++i) {
    if (!s_rpt[i].used) {
      slot = (int)i;
      break;
    }
  }
  if (slot < 0) {
    log_warn("ZMAEE_Start_Timer: %u 槽全被占用，注册失败（RE：固件直接返回）",
             (unsigned)ZM_RPT_MAX);
    return (uint32_t)-1;
  }
  if (interval_ms == 0)
    interval_ms = 1; /* 防御：0 周期会在一次事件循环里反复到期 */

  s_rpt[slot].used = true;
  s_rpt[slot].id = id;
  s_rpt[slot].cb = cb;
  s_rpt[slot].interval = interval_ms;
  s_rpt[slot].next = zm_root_get_tick(uc) + interval_ms;
  log_info("ZMAEE_Start_Timer(每 %ums, id=0x%08X, cb=0x%08X) -> 槽 %d",
           interval_ms, id, cb, slot);
  return (uint32_t)slot;
}

/* 周期表派发：一次最多派发一条（与 IShell 表同款"派发即 return"）。
 * 回调 ABI 按 RE：cb(r0=槽序号, r1=槽内参数)。固件那槽的参数字段没有写入方
 * （Start_Timer 只写 id 与 IShell 返回值），实测 applet 的回调也不读 r1
 * （0xCC2C 一进来就 bl 取全局对象），故 r1 传 0。 */
static bool zm_timer_poll_repeat(uint32_t now_ms) {
  for (uint32_t i = 0; i < ZM_RPT_MAX; ++i) {
    if (!s_rpt[i].used || !s_rpt[i].cb)
      continue;
    if ((int32_t)(now_ms - s_rpt[i].next) < 0)
      continue;
    s_rpt[i].next = now_ms + s_rpt[i].interval; /* 周期语义：接着排下一次 */
    uint32_t cb = s_rpt[i].cb;
    uint32_t r0 = i; /* RE：回调第一参 = 槽序号 */
    uint32_t r1 = 0;
    uint32_t lr = TR_enter_event_loop;
    uc_reg_write(g_uc, UC_ARM_REG_LR, &lr);
    uc_reg_write(g_uc, UC_ARM_REG_R0, &r0);
    uc_reg_write(g_uc, UC_ARM_REG_R1, &r1);
    uc_reg_write(g_uc, UC_ARM_REG_PC, &cb);
    log_info("周期定时器(槽 %u, 每 %ums) 到期 -> cb=0x%08X", i, s_rpt[i].interval,
             cb);
    return true;
  }
  return false;
}

/* =========================================================================
 * 指令级"异步中断"派发（hook_code 每 2^16 条指令调用一次）
 *
 * 见 zm_timer.h 的长注释：真机这两类定时器是 Java 层回调，和 applet 自己的
 * 事件循环无关；只靠 zm_timer_poll 会被 applet 的长自旋饿死。
 * ========================================================================= */
static bool s_int_active = false;
static uint32_t s_int_r[13]; /* R0-R12 现场 */
static uint32_t s_int_sp = 0, s_int_lr = 0, s_int_cpsr = 0, s_int_pc = 0;

/* 最近一次"applet 主动 yield（进事件循环）"的时刻。
 *
 * ★ 饥饿门限（2026-09 实测补的，很关键）：
 * 异步派发会**打断正在执行的 applet 代码**，对"一边画一边驱动状态机"的
 * applet 是致命的 —— 实测 00000506《千炮捕鱼》进关卡后必崩
 * （err=10 INSN_INVALID，PC 落到堆 0x1A014C：绘制状态被从中间打断，之后
 * 拿脏指针当函数调用）。A/B 证据：同一按键脚本下
 *   ZM_NO_ASYNC_TIMER=1 → 稳定；默认（异步开）→ 100% 崩。
 *
 * 但 00000442 那种"自旋模态循环、几十秒不回事件循环"的 applet 又确实需要
 * 异步派发（否则 1 秒定时器被饿死、计时器不涨）。
 *
 * 折中：只在 applet **长时间没 yield** 时才异步打断（默认 300ms，可用
 * ZM_ASYNC_IDLE_MS 调）。正常 yield 的 applet 永远走原来的 poll 路径，
 * 行为与加这个功能之前完全一致。 */
static uint32_t s_last_yield_ms = 0;

void zm_timer_note_yield(uint32_t now_ms) { s_last_yield_ms = now_ms; }

bool zm_timer_interrupt(uc_engine *uc, uint32_t resume_pc) {
  static int disabled = -1;
  static uint32_t idle_limit = 0;
  if (disabled < 0) {
    const char *e = getenv("ZM_NO_ASYNC_TIMER");
    disabled = (e && e[0] == '1') ? 1 : 0;
    const char *t = getenv("ZM_ASYNC_IDLE_MS");
    idle_limit = (t && atoi(t) > 0) ? (uint32_t)atoi(t) : 300u;
    if (disabled)
      log_info("定时器异步中断已禁用（ZM_NO_ASYNC_TIMER=1）");
    else
      log_info("定时器异步中断：仅当 applet 连续 %ums 未回事件循环时启用",
               idle_limit);
  }
  if (disabled || s_int_active || !uc || !resume_pc)
    return false; /* 忙：回调自己也会再触发 hook_code，别嵌套 */

  uint32_t now = zm_root_get_tick(uc);

  /* 饥饿门限：applet 还在正常 yield 就不打扰（行为与旧版一致） */
  if (s_last_yield_ms == 0) {
    s_last_yield_ms = now; /* 首次：先给它一个完整的观察窗口 */
    return false;
  }
  if ((int32_t)(now - s_last_yield_ms) < (int32_t)idle_limit)
    return false;
  uint32_t cb = 0, arg0 = 0, arg1 = 0;
  const char *what = NULL;

  /* ① 周期表（ROOT+0x148）：周期语义，先排下一次 */
  for (uint32_t i = 0; i < ZM_RPT_MAX; ++i) {
    if (!s_rpt[i].used || !s_rpt[i].cb)
      continue;
    if ((int32_t)(now - s_rpt[i].next) < 0)
      continue;
    s_rpt[i].next = now + s_rpt[i].interval;
    cb = s_rpt[i].cb;
    arg0 = i; /* RE：cb(r0=槽序号, r1=槽内参数) */
    what = "周期定时器";
    break;
  }
  /* ② IShell 单次表：同样按真机语义异步派发（到期先删，再调 cb(id, a5)） */
  if (!what) {
    for (uint32_t i = 0; i < s_timer_count; ++i) {
      if ((int32_t)(now - s_timers[i].expire) < 0)
        continue;
      uint32_t id = s_timers[i].id;
      cb = s_timers[i].cb;
      arg0 = id;
      arg1 = s_timers[i].a5;
      zm_timer_CancelTimer(NULL, id); /* 单次：先删（与 poll 一致） */
      if (cb == 0)
        continue; /* cb==0 仅移除（RE：dladdr 失败路径） */
      what = "IShell 定时器";
      break;
    }
  }
  if (!what || !cb)
    return false;

  /* 保存被打断的现场（R0-R12/SP/LR/CPSR + 恢复点 PC） */
  for (uint32_t i = 0; i < 13; ++i)
    uc_reg_read(uc, (int)(UC_ARM_REG_R0 + i), &s_int_r[i]);
  uc_reg_read(uc, UC_ARM_REG_SP, &s_int_sp);
  uc_reg_read(uc, UC_ARM_REG_LR, &s_int_lr);
  uc_reg_read(uc, UC_ARM_REG_CPSR, &s_int_cpsr);
  s_int_pc = resume_pc;

  /* 挂跳板：LR = 专用返回 trap，PC = 回调 */
  uint32_t lr = TR_timer_return;
  uc_reg_write(uc, UC_ARM_REG_LR, &lr);
  uc_reg_write(uc, UC_ARM_REG_R0, &arg0);
  uc_reg_write(uc, UC_ARM_REG_R1, &arg1);
  uc_reg_write(uc, UC_ARM_REG_PC, &cb);
  s_int_active = true;
  /* 前几次把被打断时的**模式**也打出来：回调是 ARM 代码（地址偶数），
   * 若被打断的 applet 当时在 Thumb，回调就会用错模式执行 → 非法指令。
   * 排查 0000042f 在"返回主菜单"时崩（PC=0x3A48 实测是合法 ARM `str fp,[sp]`）时加的。 */
  {
    static uint32_t n = 0;
    if (n < 3) {
      n++;
      log_info("%s 异步派发 -> cb=0x%08X(arg0=0x%X)（打断 PC=0x%X，LR=0x%X，"
               "CPSR=0x%X %s）",
               what, cb, arg0, resume_pc, s_int_lr, s_int_cpsr,
               (s_int_cpsr & 0x20) ? "Thumb" : "ARM");
    } else {
      log_info("%s 异步派发 -> cb=0x%08X(arg0=0x%X)（打断 PC=0x%X）", what, cb,
               arg0, resume_pc);
    }
  }
  return true;
}

void zm_timer_interrupt_return(uc_engine *uc) {
  if (!s_int_active)
    return;
  s_int_active = false;
  for (uint32_t i = 0; i < 13; ++i)
    uc_reg_write(uc, (int)(UC_ARM_REG_R0 + i), &s_int_r[i]);
  uc_reg_write(uc, UC_ARM_REG_SP, &s_int_sp);
  uc_reg_write(uc, UC_ARM_REG_LR, &s_int_lr);
  uc_reg_write(uc, UC_ARM_REG_CPSR, &s_int_cpsr);
  /* 最后写 PC：回到被打断的那条指令 */
  uc_reg_write(uc, UC_ARM_REG_PC, &s_int_pc);
  {
    static uint32_t n = 0;
    if (n < 3) {
      n++;
      uint32_t cpsr_now = 0;
      uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr_now);
      log_info("定时器回调返回：恢复 PC=0x%X（回调态 CPSR=0x%X %s；保存的 CPSR=0x%X）",
               s_int_pc, cpsr_now, (cpsr_now & 0x20) ? "Thumb" : "ARM",
               s_int_cpsr);
    } else {
      log_debug("定时器回调返回：现场已恢复，继续执行 PC=0x%X", s_int_pc);
    }
  }
}

/* 到期检查（RE：sub_34394 同款）。
 * 固件行为逐条复刻：
 *   - 表空：KillTimer 收摊（OS 壳层事务）+ 复位 init 标志；
 *   - 按 entry[0]→entry[k] 顺序找第一条 now>=expire 的，**派发即 return**
 *     （每 tick 最多一条）；
 *   - 取 cb=entry[4]、a5=entry[3]，先 CancelTimer(id)（单次语义）；
 *   - cb!=0 才调用，签名 cb(r0=id, r1=a5)。
 * 跳板与触摸事件同款：LR=TR_enter_event_loop，cb 返回后自动回到事件循环。 */
bool zm_timer_poll(uint32_t now_ms) {
  /* 这里是 applet 的 **yield 点**（事件循环）：记下来，供异步派发判断
   * "它是否已经长时间没回来"（见 zm_timer_interrupt 的饥饿门限）。 */
  zm_timer_note_yield(now_ms);

  /* 周期表（ROOT+0x148）先查：与 IShell 单次表相互独立 */
  if (zm_timer_poll_repeat(now_ms))
    return true;
  if (s_timer_count == 0) {
    if (s_tick_registered) {
      s_tick_registered = false; /* RE：AndroidAEE_KillTimer(dword_65A20) */
      log_info("timer 表已排空，OS 滴答收摊（下次 SetTimer 重新 init、ID 归零）");
    }
    return false;
  }
  for (uint32_t i = 0; i < s_timer_count; ++i) {
    if ((int32_t)(now_ms - s_timers[i].expire) >= 0) {
      uint32_t id = s_timers[i].id;
      uint32_t cb = s_timers[i].cb;
      uint32_t a5 = s_timers[i].a5;
      zm_timer_CancelTimer(NULL, id); /* RE：先删（单次） */
      if (cb == 0) {
        log_info("timer id=%u 到期但 cb=0，仅移除（RE：dladdr 失败路径）", id);
        return false; /* 固件：每 tick 至多处理一条 */
      }
      uint32_t lr = TR_enter_event_loop;
      uc_reg_write(g_uc, UC_ARM_REG_LR, &lr);
      uc_reg_write(g_uc, UC_ARM_REG_R0, &id);
      uc_reg_write(g_uc, UC_ARM_REG_R1, &a5);
      uc_reg_write(g_uc, UC_ARM_REG_PC, &cb);
      log_info("timer id=%u 到期 -> cb=0x%08X(id, a5=0x%08X)", id, cb, a5);
      return true; /* 事件循环 return true → Unicorn 执行 cb */
    }
  }
  return false;
}

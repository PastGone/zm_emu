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
  e->id = s_timer_next_id++;
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

/* 到期检查（RE：sub_34394 同款）。
 * 固件行为逐条复刻：
 *   - 表空：KillTimer 收摊（OS 壳层事务）+ 复位 init 标志；
 *   - 按 entry[0]→entry[k] 顺序找第一条 now>=expire 的，**派发即 return**
 *     （每 tick 最多一条）；
 *   - 取 cb=entry[4]、a5=entry[3]，先 CancelTimer(id)（单次语义）；
 *   - cb!=0 才调用，签名 cb(r0=id, r1=a5)。
 * 跳板与触摸事件同款：LR=TR_enter_event_loop，cb 返回后自动回到事件循环。 */
bool zm_timer_poll(uint32_t now_ms) {
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

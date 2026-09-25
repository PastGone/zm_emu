#ifndef ZM_TIMER_H
#define ZM_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/* ZMAEE IShell 定时器子系统（RE 全集：ZMAEE_IShell_SetTimer /
 * CancelTimer / CancelOwnerTimer / 派发 sub_34394 / 收摊
 * AndroidAEE_KillTimer），从 zm_shell 拆出独立成模块。
 *
 * 固件表结构（0x65A18 起 0x14C 字节）：
 *   table[0]      ID 计数器（递增分配）
 *   table[1]      当前条目数（SetTimer 检查 >15 返 -1，即最多 16 条）
 *   table[2]      OS 滴答句柄（AndroidAEE 壳层 8 槽，模拟器不复刻，
 *                 由宿主事件循环轮询替代）
 *   entry[k] = table[3+5k .. 7+5k]：
 *     [0]=ID  [1]=owner  [2]=expire=GetTickCount()+dur  [3]=a5  [4]=cb
 */

/* +0x3C SetTimer(this, dur_ms, cb, owner, a5)（RE）
 * ID 递增分配不复用；表满（count>15）返 -1。
 * init 块语义（dword_65C64==0 时 memset 全表）：首次注册或"排空后
 * 重新启用"时 ID 从 0 重新计数。
 * 单次语义：到期先 CancelTimer(id) 再调 cb(id, a5)；周期定时器需在
 * 回调里重新 SetTimer（applet 本就如此使用）。 */
uint32_t zm_timer_SetTimer(uc_engine *uc, uint32_t dur_ms, uint32_t cb,
                           uint32_t owner, uint32_t a5);

/* +0x40 CancelTimer(this, timer_id)（RE）：按 entry[0]==ID 查找，
 * 找到后左侧整体前移压缩、数量-1、清 vacated 尾槽；成功返 0，
 * 未找到/空表返 -1。 */
uint32_t zm_timer_CancelTimer(uc_engine *uc, uint32_t timer_id);

/* +0x44 CancelOwnerTimer(this, owner)（RE）：遍历删除全部 entry[1]==owner
 * 的定时器（逐个转 CancelTimer 按 ID 删），恒返 0。
 * 注意保真固件怪癖：删除后从下一槽继续扫（滑下来的同 owner 条目
 * 会被跳过、留在表里）。 */
uint32_t zm_timer_CancelOwnerTimer(uc_engine *uc, uint32_t owner);

/* 事件循环每帧调用：检查到期定时器（RE：sub_34394 同款）。
 * 每次最多派发一条（固件行为：派发即 return）；派发 = 先 CancelTimer
 * 按 ID 移除（单次语义），再经触摸事件同款跳板调 cb(r0=id, r1=a5)，
 * LR 指回 TR_enter_event_loop 使回调返回后回到事件循环。
 * 返回 true 表示已挂上跳板（寄存器已写好），事件循环应立即 return true
 * 让模拟器执行 cb；false 表示本帧无事。
 * cb==0 时仅移除不调用（RE：dladdr 失败静默跳过；dladdr 是 Android
 * 移植壳的符号校验，客户机扁平内存下无意义，不复刻）。
 * 表排空时收摊（RE：AndroidAEE_KillTimer + dword_65C64=0 → 下次
 * SetTimer 重新 init、ID 归零）。 */
bool zm_timer_poll(uint32_t now_ms);

/* ---- ROOT+0x148 / +0x14C：固件的"applet 级**周期**定时器"（RE 全集） ----
 *
 * RE（libaee.so 符号表 + 反汇编）：
 *   ZMAEE_Start_Timer(interval_ms, id, cb)   @0x2E4E0
 *     → 内部第一件事就是 bl ZMAEE_Stop_Timer(id)（同 id 重注册 = 先停旧的）
 *     → 再从 8 槽表里找空槽（全占用就直接返回）
 *     → 然后 bl ZMAEE_IShell_SetTimer（0x34308 = 我们已实现的同一个函数）
 *     → 把 (id, 返回值) 存进槽
 *   ZMAEE_Stop_Timer(id)                     @0x2E498
 *     → 在 8 槽里按 id 匹配（比较 [+0xC]/[+0x14]/[+0x1C]/[+0x24]）后清槽
 *   nativeAEETimerCallback()                 @0x22E88
 *     → 每次滴答遍历 8 槽，对已注册的槽**调用 cb(r0=槽序号, r1=槽内参数)**
 *
 * 与 IShell 那套的区别：IShell_SetTimer 是**单次**（到期先删再调、要周期得
 * 自己在回调里重注册），这两个 API 是**周期**语义，所以单独一张表。
 *
 * 实测症状（00000442《驱蚊大师》）：
 *   进游戏时 applet 调 Stop_Timer(0x13AC4) + Start_Timer(1000, 0x13AC4,
 *   0xCC2C)，而 0xCC2C 就是"计时数字进位"的回调（个位 +1，超 '9' 归 '0'
 *   进位，秒十位超 '5' 进位到分 —— 码表式累加）。两个槽原先都未接线 →
 *   回调一次都不触发 → 画面上的
 *     "执行时间：00:00:00 / 倒数计时：00:01:00"
 *   永远不动（时钟文字是 DrawText 画的，所以字显示正常、只是数值不涨）。 */
uint32_t zm_timer_StartTimer(uc_engine *uc, uint32_t interval_ms, uint32_t id,
                             uint32_t cb);
uint32_t zm_timer_StopTimer(uc_engine *uc, uint32_t id);

/* ---- 指令级"异步中断"派发（hook_code 调用）------------------------------
 *
 * 为什么需要：真机这两类定时器由 **Java 层周期性回调**触发，与 applet 自己
 * 的事件循环无关；而宿主只在 applet 主动回事件循环时才调 zm_timer_poll。
 * 实测 00000442《驱蚊大师》：进游戏后 applet 是"自旋 + 绘制"的模态循环，
 * 54 秒只回事件循环 3 次 → 1 秒定时器与 500ms 心跳双双被饿死（计时器停住、
 * 游戏状态机不推进）。
 *
 * 所以补一条指令级路径：到期的回调直接**打断**当前执行 ——
 *   保存 R0-R12/SP/LR/CPSR 与"恢复点 PC" →  LR = TR_timer_return（专用跳板）
 *   →  PC = 回调；回调 `bx lr` 落回跳板后由 zm_timer_interrupt_return 恢复
 *   现场，继续执行被打断的那条指令。
 *
 * 调用点：hook.c 的 hook_code 每 2^16 条指令问一次（开销可忽略）。
 * 返回 true 表示已挂上跳板（PC 已改写），调用方应立即 return。
 * 回调执行期间会置忙标志，避免回调自己再被嵌套中断。
 * 环境变量 ZM_NO_ASYNC_TIMER=1 可关掉这条路径（A/B 对比用）。 */
bool zm_timer_interrupt(uc_engine *uc, uint32_t resume_pc);
void zm_timer_interrupt_return(uc_engine *uc);

/* 通用异步跳板：把 guest 打断在 resume_pc，去执行 cb(r0..r{nargs-1})。
 * args 最多 5 个（R0-R4，zmaee 的事件回调约定要 R4=this，所以触摸用 5 个）。
 * 与 zm_timer_interrupt 共用同一套现场保存/恢复（TR_timer_return），
 * 所以同一时刻只能有一个在飞（s_int_active 忙标志会挡住第二个）。
 * 触摸事件的异步派发（event.c 的 zm_event_async_poll）走这个入口。 */
bool zm_timer_async_call(uc_engine *uc, uint32_t resume_pc, const char *what,
                         uint32_t cb, const uint32_t *args, int nargs);

/* "applet 是否已经饿到可以被异步打断"：连续 idle_limit（默认 300ms，
 * ZM_ASYNC_IDLE_MS 可调）没回过事件循环。触摸事件用它做同样的门限判断
 * —— 正常 yield 的 applet 永远走事件循环那条路，不会被异步打断。
 * ZM_NO_ASYNC_TIMER=1 会一并关掉（A/B 对比用）。 */
bool zm_timer_is_starved(uc_engine *uc);

/* 由 zm_timer_poll 在**每次 applet yield（进事件循环）**时调用，记录时刻。
 *
 * 异步派发靠它做饥饿判断：applet 只要还在正常 yield，就绝不打断它
 * （实测 00000506 被异步打断会崩、00000442 的模态循环则必须靠异步喂）。
 * 细节见 zm_timer.c 的 s_last_yield_ms 注释。 */
void zm_timer_note_yield(uint32_t now_ms);

#endif /* ZM_TIMER_H */

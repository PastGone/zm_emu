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

#endif /* ZM_TIMER_H */

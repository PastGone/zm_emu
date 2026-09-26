#ifndef ZM_STAT_H
#define ZM_STAT_H

#include <stdint.h>

/* 调用/点击统计探针（ZM_STAT=1 开启，默认关闭）。
 *
 * 目的：排查"哪个东西被疯狂调用/疯狂点击"这类问题 ——
 *   1) 每个外部调用槽（trap，形如 +0x18）被调了多少次；
 *   2) 每个触摸坐标被点了多少次（哪块 UI 被反复点）。
 * 退出时按次数打印 Top 榜（见 zm_stat_dump）。 */
void zm_stat_init(void);			   /* 读 ZM_STAT 决定是否启用 */
void zm_stat_trap(uint32_t trap_addr); /* handle_trap 每次调用一次 */
void zm_stat_touch(uint32_t x, uint32_t y);
void zm_stat_dump(void); /* 打印 Top 榜（进程收尾时调一次） */

#endif

#ifndef EMU_H
#define EMU_H

#include <stddef.h>
#include <stdint.h>

#include "./tool/paser_info.h"
#include <capstone/capstone.h>
#include <unicorn/unicorn.h>

/* 子模块拆分（纯常量 / root 分发表 / 各对象虚表，均不依赖本文件） */
#include "emu_mem_layout.h" /* BLOB/STACK/HEAP/SHIM 布局 + 对象/虚表地址 + TRAP 宏 */
#include "emu_root_traps.h" /* ROOT_TABLE_ADDR 枚举 + TR_root_* 宏 */
#include "emu_vt_traps.h"   /* shell/file/filemgr/media/setting/display/image/bitmap/surf 等虚表 trap */

// -------------------- 全局变量 --------------------
extern uc_engine *g_uc;
extern AppletHeader g_header;
extern uint32_t g_heap_ptr;

/**
 * 客户机堆策略（由 ZM_ULIBC_HEAP 环境变量在 zm_emu_map_memory 中设定）。
 *
 *   1（默认）ulibc 真实堆：u_malloc / u_free，带空闲链表与相邻块合并。
 *            内存真正回收复用，这是 applet 应该跑在的正确环境——
 *            原始固件上 malloc/free 也是真的回收的。
 *            代价：会暴露 applet 潜伏的 UAF / double-free。
 *            但那些是 applet 的真实 bug，本就该暴露，而不是被掩盖。
 *
 *   0         bump 分配器：host_malloc 单调递增，free 为空操作。
 *             **仅用于排查**：当某个 applet 在真实堆下崩溃时，
 *             切到 0 可以确认"崩溃是由内存回收引起的"（即 applet 有
 *             UAF/double-free），而非模拟器接线本身的问题。
 *             长期保留会让内存只增不减，不是正确行为。
 */
extern int g_ulibc_heap;

extern uint32_t g_instance;
extern uint32_t g_handler;
/* applet 通过 ROOT_TABLE_ADDR+0x1184 注册的事件循环入口（0 = 未注册） */
extern uint32_t g_registered_loop;
extern int g_trap_pause;
extern int g_disasm;

/* 当前载入 applet 的短名称（如 "00000102.app"），由 main.c 设置，
 * 供 TR_init_callback 写入 applet instance+4。 */
extern char
    g_app_pathname[4096]; // 4096是 linux
                          // 的最长文件名,这里设了这么大是为了防止搞什么摇蛾子

// 用来反汇编用的一组全局变量，之所以是全局变量是因为要不停的复用
extern csh g_cs_handle;
extern cs_insn *g_sc_insn;
extern size_t g_sc_count;
extern uint8_t g_cscode[16];

// -------------------- 函数声明 --------------------

/* 构建所有虚表：把 trap 地址写入客户机虚拟内存中的 shim 区 */
int zm_emu_build_vtables();

/* Unicorn 内存映射（blob/stack/heap/shim/tramp/zmr） */
int zm_emu_map_memory();

/* 载入 applet blob 到 BLOB_BASE */
int zm_emu_load_blob(FILE *fp, const long *applet_size);

/* 注册 Unicorn 钩子（code / unmapped mem / shim mem） */
int zm_emu_add_hooks();

/* 设置初始寄存器，启动 applet（init → 绘制 → 停止） */
int zm_emu_start_applet();

#endif

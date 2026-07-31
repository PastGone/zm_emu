#ifndef __EMU_H__
#define __EMU_H__

#include <stddef.h>
#include <stdint.h>

#include "./tool/paser_info.h"
#include <capstone/capstone.h>
#include <unicorn/unicorn.h>

// -------------------- 内存布局常量 --------------------
#define ONE_MB (0x100000)
#define HALF_MB (0x80000)
//
#define BLOB_BASE (0x80000)
#define BLOB_SIZE (1 * ONE_MB)

#define STACK_BASE (BLOB_BASE + BLOB_SIZE)
#define STACK_SIZE (1 * HALF_MB)
#define STACK_TOP (STACK_BASE + STACK_SIZE)

#define HEAP_BASE (STACK_TOP + ONE_MB / 8)
#define HEAP_SIZE (6 * ONE_MB)
#define HEAP_END (HEAP_BASE + HEAP_SIZE)

#define SHIM_BASE (HEAP_END)
#define SHIM_SIZE (1 * HALF_MB)

#define TRAMP_BASE (SHIM_BASE + SHIM_SIZE)
#define TRAMP_SIZE (1 * HALF_MB)

#define ROOT_SLOT_OFF 0x180

#define APPLET_ENTRY_OFF 0x188
#define APPLET_ENTRY_POINT (BLOB_BASE + APPLET_ENTRY_OFF)

// -------------------- trap 地址宏 --------------------

// -------------------- 外部函数 trap 地址 --------------------
#define TRAP(idx) (TRAMP_BASE + 4 * (idx))

/* -------------------- trap 地址定义 -------------------- */
#define TR_root_queryRuntime TRAP(0)
#define TR_root_malloc TRAP(1)
#define TR_root_free TRAP(2)
#define TR_root_str_copy TRAP(3)
#define TR_root_sprintf TRAP(4)
#define TR_root_str_ctor TRAP(5)
#define TR_root_spec_lookup TRAP(6)
#define TR_root_str_find TRAP(7)

#define TR_rt_queryInterface TRAP(8)
#define TR_rt_getSystemInfo TRAP(9)

#define TR_gfx_clear TRAP(10)
#define TR_gfx_fillRect TRAP(11)
#define TR_gfx_commit TRAP(12)
#define TR_gfx_drawText TRAP(13)
#define TR_gfx_drawRect TRAP(14)
#define TR_gfx_fillRect2 TRAP(15)

#define TR_fs_open TRAP(16)
#define TR_file_close TRAP(17)
#define TR_file_read TRAP(18)
#define TR_file_seek TRAP(19)
#define TR_file_size TRAP(58) /* FILE_VT[0x24]：返回文件总大小 */

#define TR_audio_stop TRAP(20)
#define TR_ap_play TRAP(21)
#define TR_ap_stop TRAP(22)
#define TR_audio_get_status TRAP(60) /* AUDIO_VT[0x24] */

/* 00000405.app：FS vtable 缺失槽 */
#define TR_fs_enum TRAP(74) /* FS_VT[0x30]：enumFile */

/* 00000405.app 新增 ROOT vtable trap（索引 23..45） */

#define TR_root_memset TRAP(28) /* ROOT[0x60] */
#define TR_root_x74 TRAP(29)
#define TR_root_str_assign TRAP(30) /* ROOT[0x78] */
#define TR_root_get_tick TRAP(40)   /* ROOT[0xD8] */
#define TR_root_x12C TRAP(41)
#define TR_root_x130 TRAP(42)
#define TR_root_x140 TRAP(43)
#define TR_root_create_cbk TRAP(44) /* ROOT[0x154] */
#define TR_root_x16C TRAP(45)

/* 服务对象 / FS / RT / DLL / CBK trap（索引 46..57） */
#define TR_svc_release TRAP(46)
#define TR_svc04_x1C TRAP(47)
#define TR_svc09_x2C TRAP(48)
#define TR_svc09_x40 TRAP(49)
#define TR_fs_release TRAP(50)
#define TR_fs_chdir TRAP(51)
#define TR_rt_loadDLL TRAP(52)
#define TR_rt_unloadDLL TRAP(53)
#define TR_rt_loadDLL2 TRAP(59) /* RT_VT[0x78] */
#define TR_dll_init TRAP(54)
#define TR_dll_config TRAP(55)
#define TR_dll_entry TRAP(56)
#define TR_cbk_default TRAP(57)

//
#define TR_init_callback                                                       \
  TRAP(114514) // 随便写一个位置我想也应该不影响这个叫什么来.初始化回调

// 事件回调因为 apple 是没有主循环的所以要用外部来完成这个主循环
#define TR_event_callback TRAP(0721)

extern uint32_t SIZE_SLOT;
extern uint32_t API_SLOT;

// -------------------- 全局变量 --------------------
extern uc_engine *uc;
extern AppletHeader header;
extern uint32_t heap_ptr;

extern uint32_t g_instance;
extern uint32_t g_handler;
extern int g_trap_pause;
extern int g_disasm;

/* 当前载入 applet 的短名称（如 "00000102.app"），由 main.c 设置，
 * 供 TR_init_callback 写入 applet instance+4。 */
extern char
    g_app_pathname[4096]; // 4096是 linux
                          // 的最长文件名,这里设了这么大是为了防止搞什么摇蛾子

extern csh cs_handle;
extern cs_insn *insn;
extern size_t count;
extern uint8_t code[16];

// -------------------- 函数声明 --------------------

/* 构建所有虚表：把 trap 地址写入客户机虚拟内存中的 shim 区 */
int zm_emu_build_vtables(uc_engine *uc);

/* Unicorn 内存映射（blob/stack/heap/shim/tramp/zmr） */
int zm_emu_map_memory(uc_engine *uc);

/* 载入 applet blob 到 BLOB_BASE */
int zm_emu_load_blob(uc_engine *uc, FILE *fp, long *applet_size);

/* 注册 Unicorn 钩子（code / unmapped mem / shim mem） */
int zm_emu_add_hooks(uc_engine *uc);

/* 设置初始寄存器，启动 applet（init → 绘制 → 停止） */
int zm_emu_start_applet(uc_engine *uc);

#endif
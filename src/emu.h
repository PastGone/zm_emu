#ifndef EMU_H
#define EMU_H

#include <stddef.h>
#include <stdint.h>

#include "./tool/paser_info.h"
#include <capstone/capstone.h>
#include <unicorn/unicorn.h>

// -------------------- 内存布局常量 --------------------
#define ONE_MB (0x100000U)
#define HALF_MB (0x80000U)
//
#define BLOB_BASE (0x80000U)
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

//
#define ROOT_SLOT_OFF 0x180U
#define APPLET_ENTRY_OFF 0x188U
#define APPLET_ENTRY_POINT (BLOB_BASE + APPLET_ENTRY_OFF)

/* -------------------- shim 虚表地址定义 -------------------- */
#define ROOT (SHIM_BASE + 0x000U)
#define SHELL (SHIM_BASE + 0x100U)
#define RT_VT (SHIM_BASE + 0x180U)
#define GFX (SHIM_BASE + 0x200U)
#define GFX_VT (SHIM_BASE + 0x280U)
#define FileMgr (SHIM_BASE + 0x300U)
#define FileMgr_VT (SHIM_BASE + 0x380U)
#define FILE1 (SHIM_BASE + 0x400U)
#define FILE_VT (SHIM_BASE + 0x480U)
#define AUDIO (SHIM_BASE + 0x500U)
#define AUDIO_VT (SHIM_BASE + 0x580U)
#define AP (SHIM_BASE + 0x600U)
#define AP_VT (SHIM_BASE + 0x680U)
#define DUMMY_BUF (SHIM_BASE + 0x750U)

//

/* 00000405.app 新增 shim 对象地址（0x800 起，与 DUMMY_BUF@0x750 不冲突） */
#define INIT_CTX (SHIM_BASE + 0x800U) /* 256B 零填充：init 事件 r3 上下文 */
#define SVC04 (SHIM_BASE + 0x900U)    /* 0x1000004 服务对象 */
#define SVC04_VT (SHIM_BASE + 0x910U)
#define SVC09 (SHIM_BASE + 0x940U) /* 0x1000009 服务对象 */
#define SVC09_VT (SHIM_BASE + 0x950U)
#define CBK_OBJ (SHIM_BASE + 0x980U)    /* sub_84E04 返回的回调对象 */
#define CBK_OBJ_VT (SHIM_BASE + 0x990U) /* 可写：applet 覆写 vt[+8] */
#define DLL_OBJ (SHIM_BASE + 0x9C0U)    /* loadDLL 返回的 stub DLL 对象 */
#define DLL_OBJ_VT (SHIM_BASE + 0x9D0U)

//
#define SIZE_SLOT                                                              \
  (SHIM_BASE + 0x700U) // 其实这个文件大小槽还有待确认，applet 会写入这个槽
#define API_SLOT                                                               \
  (SHIM_BASE + 0x710U) // applet 会写入这个槽，用于调用 applet 接口的 vt

// -------------------- trap 地址宏 --------------------
#define TRAP(idx) (TRAMP_BASE + (idx) - SHIM_BASE)

// -------------------- 外部函数 trap 地址 --------------------

/* -------------------- trap 地址定义 -------------------- */
// root

#define TR_root_getShell TRAP(ROOT + 0x00U)
#define TR_root_malloc TRAP(ROOT + 0x08U)
#define TR_root_free TRAP(ROOT + 0x0cU)
#define TR_root_str_copy TRAP(ROOT + 0x20U)
#define TR_root_sprintf TRAP(ROOT + 0x6cU)
#define TR_root_str_ctor TRAP(ROOT + 0x88U)
#define TR_root_spec_lookup TRAP(ROOT + 0xa4U)
#define TR_root_str_find TRAP(ROOT + 0xa8U)
// runtime
/*


*/
#define TR_rt_queryInterface TRAP(RT_VT + 0x08U)
#define TR_rt_getSystemInfo TRAP(RT_VT + 0x10U)
// gfx

#define TR_gfx_clear TRAP(GFX_VT + 0x20U)
#define TR_gfx_fillRect TRAP(GFX_VT + 0x2CU)
#define TR_gfx_commit TRAP(GFX_VT + 0x40U)
#define TR_gfx_drawText TRAP(GFX_VT + 0x50U)
#define TR_gfx_drawRect TRAP(GFX_VT + 0x6CU)
#define TR_gfx_fillRect2 TRAP(GFX_VT + 0x70U)
// fs

#define TR_fileMgr_open_file TRAP(FileMgr_VT + 0x08U)
#define TR_file_close TRAP(FILE_VT + 0x04U)
#define TR_file_read TRAP(FILE_VT + 0x08U)
#define TR_file_seek TRAP(FILE_VT + 0x20U)
#define TR_file_size TRAP(FILE_VT + 0x24U) /* FILE[0x24]：返回文件总大小 */
// audio

#define TR_audio_stop TRAP(AUDIO_VT + 0x14U)
#define TR_ap_play TRAP(AP_VT + 0x10U)
#define TR_ap_stop TRAP(AP_VT + 0x14U)
#define TR_audio_get_status TRAP(AUDIO_VT + 0x24U) /* AUDIO_VT[0x24] */

/* 00000405.app：FS vtable 缺失槽 */
#define TR_fs_enum TRAP(FS_VT + 0x30U) /* FS_VT[0x30]：enumFile */

/* 00000405.app 新增 ROOT vtable trap（索引 23..45） */

#define TR_root_memset TRAP(ROOT + 0x28U) /* ROOT[0x60] */
#define TR_root_x74 TRAP(ROOT + 0x29U)
#define TR_root_str_assign TRAP(ROOT + 0x30U) /* ROOT[0x78] */
#define TR_root_get_tick TRAP(ROOT + 0xD8U)   /* ROOT[0xD8] */
#define TR_root_x12C TRAP(ROOT + 0x12CU)
#define TR_root_x130 TRAP(ROOT + 0x130U)
#define TR_root_x140 TRAP(ROOT + 0x140U)
#define TR_root_create_cbk TRAP(ROOT + 0x154U)
#define TR_root_x16C TRAP(ROOT + 0x16CU)

/* 服务对象 / FS / RT / DLL / CBK trap（索引 46..57） */
#define TR_svc_release TRAP(ROOT + 0x2EU)
#define TR_svc04_x1C TRAP(ROOT + 0x2FU)
#define TR_svc09_x2C TRAP(ROOT + 0x30U)
#define TR_svc09_x40 TRAP(ROOT + 0x31U)
#define TR_fs_release TRAP(ROOT + 0x32U)
#define TR_fs_chdir TRAP(ROOT + 0x33U)
#define TR_rt_loadDLL TRAP(ROOT + 0x34U)
#define TR_rt_unloadDLL TRAP(ROOT + 0x35U)
#define TR_rt_loadDLL2 TRAP(ROOT + 0x3BU) /* RT_VT[0x78] */
#define TR_dll_init TRAP(ROOT + 0x3CU)
#define TR_dll_config TRAP(ROOT + 0x3DU)
#define TR_dll_entry TRAP(ROOT + 0x3EU)
#define TR_cbk_default TRAP(ROOT + 0x3FU)

//
#define TR_init_callback                                                       \
  TRAP(ROOT + 0x118cU) // 随便写一个位置我想也应该不影响这个叫什么来.初始化回调

// 事件回调因为 apple 是没有主循环的所以要用外部来完成这个主循环
#define TR_enter_event_loop TRAP(ROOT + 0x1180U)

// -------------------- 全局变量 --------------------
extern uc_engine *g_uc;
extern AppletHeader g_header;
extern uint32_t g_heap_ptr;

extern uint32_t g_instance;
extern uint32_t g_handler;
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
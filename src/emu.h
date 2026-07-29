#ifndef __EMU_H__
#define __EMU_H__

#include <stddef.h>
#include <stdint.h>

#include "./tool/paser_info.h"
#include <capstone/capstone.h>
#include <unicorn/unicorn.h>

// -------------------- 内存布局常量 --------------------
#define ONE_MB (0x100000)
#define BLOB_BASE (0x80000)
#define BLOB_SIZE (1 * ONE_MB)

#define STACK_BASE (BLOB_BASE + BLOB_SIZE)
#define STACK_SIZE (1 * ONE_MB)
#define STACK_TOP (STACK_BASE + STACK_SIZE)

#define HEAP_BASE (STACK_TOP + ONE_MB / 8)
#define HEAP_SIZE (6 * ONE_MB)
#define HEAP_END (HEAP_BASE + HEAP_SIZE)

#define SHIM_BASE (HEAP_END)
#define SHIM_SIZE (1 * ONE_MB)

#define TRAMP_BASE (SHIM_BASE + SHIM_SIZE)
#define TRAMP_SIZE (1 * ONE_MB)

#define ZMR_BASE (TRAMP_BASE + TRAMP_SIZE)
#define ZMR_SIZE (2 * ONE_MB)

#define ROOT_SLOT_OFF 0x180

#define APPLET_ENTRY_OFF 0x188
#define APPLET_ENTRY_POINT (BLOB_BASE + APPLET_ENTRY_OFF)

// -------------------- trap 地址宏 --------------------
#define TRAP(idx) (TRAMP_BASE + 4 * (idx))

// -------------------- 外部函数 trap 地址 --------------------
extern uint32_t TR_root_queryRuntime;
extern uint32_t TR_root_malloc;
extern uint32_t TR_root_free;
extern uint32_t TR_root_str_copy;
extern uint32_t TR_root_sprintf;
extern uint32_t TR_root_str_ctor;
extern uint32_t TR_root_spec_lookup;
extern uint32_t TR_root_str_find;

extern uint32_t TR_rt_queryInterface;
extern uint32_t TR_rt_getSystemInfo;

extern uint32_t TR_gfx_clear;
extern uint32_t TR_gfx_fillRect;
extern uint32_t TR_gfx_commit;
extern uint32_t TR_gfx_drawText;
extern uint32_t TR_gfx_drawRect;
extern uint32_t TR_gfx_fillRect2;

extern uint32_t TR_fs_open;
extern uint32_t TR_file_close;
extern uint32_t TR_file_read;
extern uint32_t TR_file_seek;

extern uint32_t TR_audio_stop;
extern uint32_t TR_ap_play;
extern uint32_t TR_ap_stop;

extern uint32_t TR_init_callback;
extern uint32_t SIZE_SLOT;
extern uint32_t API_SLOT;

// -------------------- 全局变量 --------------------
extern uc_engine *uc;
extern AppHeader header;
extern uint32_t heap_ptr;

extern uint32_t g_instance;
extern uint32_t g_handler;
extern int g_trap_pause;
extern int g_disasm;

extern csh handle;
extern cs_insn *insn;
extern size_t count;
extern uint8_t code[16];

// -------------------- 函数声明 --------------------

/* 构建所有虚表：把 trap 地址写入客户机虚拟内存中的 shim 区 */
int zm_emu_build_vtables(uc_engine *uc);

/* Unicorn 内存映射（blob/stack/heap/shim/tramp/zmr） */
int zm_emu_map_memory(uc_engine *uc);

/* 载入 applet blob 到 BLOB_BASE */
int zm_emu_load_blob(uc_engine *uc, const char *filename, long *out_size);

/* 载入 .zmr 资源（按 .app 路径替换后缀） */
void zm_emu_load_zmr_if_exists(uc_engine *uc, const char *app_path);

/* 注册 Unicorn 钩子（code / unmapped mem / shim mem） */
int zm_emu_add_hooks(uc_engine *uc, uc_hook *hook_code_h,
                     uc_hook *hook_unmapped_h, uc_hook *hook_shim_h);

/* 设置初始寄存器，启动 applet（init → 绘制 → 停止） */
int zm_emu_start_applet(uc_engine *uc);

#endif
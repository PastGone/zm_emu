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
extern uint32_t TR_file_size; /* FILE_VT[0x24] */

extern uint32_t TR_audio_stop;
extern uint32_t TR_ap_play;
extern uint32_t TR_ap_stop;
extern uint32_t TR_audio_get_status; /* AUDIO_VT[0x24] */

/* ---- 00000405.app：GFX vtable 缺失槽（索引 61..70） ---- */
extern uint32_t TR_gfx_x18; /* GFX_VT[0x18] */
extern uint32_t TR_gfx_x34; /* GFX_VT[0x34] */
extern uint32_t TR_gfx_x38; /* GFX_VT[0x38] */
extern uint32_t TR_gfx_x44; /* GFX_VT[0x44] */
extern uint32_t TR_gfx_x48; /* GFX_VT[0x48]：返回屏幕宽度 */
extern uint32_t TR_gfx_x54; /* GFX_VT[0x54] */
extern uint32_t TR_gfx_x68; /* GFX_VT[0x68] */
extern uint32_t TR_gfx_x94; /* GFX_VT[0x94] */
extern uint32_t TR_gfx_xA4; /* GFX_VT[0xA4] */
extern uint32_t TR_gfx_xB0; /* GFX_VT[0xB0] */

/* 00000405.app：GFX vtable 补充缺失槽（索引 71..73） */
extern uint32_t TR_gfx_x0C; /* GFX_VT[0x0C]：setClipRect / fillRect */
extern uint32_t TR_gfx_x28; /* GFX_VT[0x28]：flush / commitRegion */
extern uint32_t TR_gfx_x4C; /* GFX_VT[0x4C]：measureChar（文本测量） */

/* 00000405.app：FS vtable 缺失槽 */
extern uint32_t
    TR_fs_enum; /* FS_VT[0x30]：enumFile(FS, index) → 返 0 表无文件 */

extern uint32_t TR_init_callback;
extern uint32_t SIZE_SLOT;
extern uint32_t API_SLOT;

/* ---- 00000405.app 新增 ROOT vtable trap（索引 23..45） ---- */
extern uint32_t TR_root_x18;
extern uint32_t TR_root_x1C;
extern uint32_t TR_root_x24;
extern uint32_t TR_root_x50;
extern uint32_t TR_root_x5C;
extern uint32_t TR_root_memset; /* ROOT[0x60] */
extern uint32_t TR_root_x74;
extern uint32_t TR_root_str_assign; /* ROOT[0x78] */
extern uint32_t TR_root_x7C;
extern uint32_t TR_root_x80;
extern uint32_t TR_root_x84;
extern uint32_t TR_root_x8C;
extern uint32_t TR_root_x90;
extern uint32_t TR_root_xB0;
extern uint32_t TR_root_xC0;
extern uint32_t TR_root_xC8;
extern uint32_t TR_root_xD0;
extern uint32_t TR_root_get_tick; /* ROOT[0xD8] -> SDL_GetTicks() */
extern uint32_t TR_root_x12C;
extern uint32_t TR_root_x130;
extern uint32_t TR_root_x140;
extern uint32_t TR_root_create_cbk; /* ROOT[0x154] -> CBK_OBJ */
extern uint32_t TR_root_x16C;

/* ---- 服务对象 / FS / RT / DLL / CBK trap（索引 46..57） ---- */
extern uint32_t TR_svc_release;
extern uint32_t TR_svc04_x1C;
extern uint32_t TR_svc09_x2C;
extern uint32_t TR_svc09_x40;
extern uint32_t TR_fs_release;
extern uint32_t TR_fs_chdir;
extern uint32_t TR_rt_loadDLL;
extern uint32_t TR_rt_unloadDLL;
extern uint32_t TR_rt_loadDLL2; /* RT_VT[0x78]：sub_83E24 调用的模块载入变体 */
extern uint32_t TR_dll_init;
extern uint32_t TR_dll_config;
extern uint32_t TR_dll_entry;
extern uint32_t TR_cbk_default;

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

/* 载入 .zmr 资源（按 .app 路径替换后缀） */
void zm_emu_load_zmr_if_exists(uc_engine *uc, const char *app_path);

/* 注册 Unicorn 钩子（code / unmapped mem / shim mem） */
int zm_emu_add_hooks(uc_engine *uc);

/* 设置初始寄存器，启动 applet（init → 绘制 → 停止） */
int zm_emu_start_applet(uc_engine *uc);

#endif
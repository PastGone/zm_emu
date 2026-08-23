#ifndef EMU_H
#define EMU_H

#include <stdbool.h>
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
#define BLOB_SIZE (4 * ONE_MB) /* 最大的 applet(00000440) 约 260KB，留足余量 */

#define STACK_BASE (BLOB_BASE + BLOB_SIZE)
#define STACK_SIZE (2 * ONE_MB)
#define STACK_TOP (STACK_BASE + STACK_SIZE)

#define HEAP_BASE (STACK_TOP + ONE_MB / 8)
/* 00000440（640x640 的 SLG）会反复申请 4MB 级资源缓冲，48MB 不够用；
 * 客户机是 32 位地址空间，这里给到 256MB 仍留有充足余量。 */
#define HEAP_SIZE (256 * ONE_MB)
#define HEAP_END (HEAP_BASE + HEAP_SIZE)

#define SHIM_BASE (HEAP_END)
#define SHIM_SIZE (1 * HALF_MB)

#define TRAMP_BASE (SHIM_BASE + SHIM_SIZE)
#define TRAMP_SIZE (1 * HALF_MB)

/* 显存区：图层缓冲与图片像素专用。
 * 单独划一块而不是跟 applet 抢客户机堆——00000440 会把 48MB 堆全部吃光，
 * 之后 640x640 的图层就分配不出来，画面直接全黑。 */
#define VRAM_BASE (TRAMP_BASE + TRAMP_SIZE)
#define VRAM_SIZE (64 * ONE_MB)
#define VRAM_END (VRAM_BASE + VRAM_SIZE)

/* uc_emu_start 的"停止地址"哨兵：必须是一个绝不会被 applet 执行到的地址。
 * 不能等于 0（Unicorn 会把 until==0 视为"PC 到 0 即停"，会吃掉空函数指针调用）。 */
#define EMU_STOP_SENTINEL ((uint64_t)0xFFFFFFFFull)

//
#define ROOT_SLOT_OFF 0x180U
#define APPLET_ENTRY_OFF 0x188U
#define APPLET_ENTRY_POINT (BLOB_BASE + APPLET_ENTRY_OFF)

/* -------------------- shim 区对象布局 --------------------
 *
 * SHIM 区里每 4 字节都被预填成"指向 TRAMP 区同偏移地址"的函数指针，
 * applet 取出后跳转即触发 hook_code → handle_trap。
 *
 * 因此这里必须给每个对象 / 虚表划分【互不重叠】的独立区域：
 *   - ROOT 是一张【直接函数指针表】（applet 用 (*(ROOT+off))(...) 调用），
 *     实测偏移会用到 0x150 以上，故独占 0x1000 字节；
 *   - XXX 是【对象】：word0 = 指向 XXX_VT 的指针，其余字段清零；
 *   - XXX_VT 是【虚表】：整块保持 trap 指针。
 *
 * 旧布局把 ROOT(0x000，长度仅 0x100) 与 RUNTIME(0x100)/RT_VT(0x180)/GFX(0x200)
 * 挤在一起，ROOT[0x100] 之后的槽位会被对象的 vtable 指针覆盖，
 * 导致 applet 调 ROOT[0x154] 之类时跳到错误地址。此处按 0x1000 对齐重排。
 */
#define ZM_OBJ_STRIDE 0x1000U

#define ROOT (SHIM_BASE + 0x00000U) /* 直接函数指针表，预留 4KB */
#define RUNTIME (SHIM_BASE + 0x01000U)
#define RT_VT (SHIM_BASE + 0x02000U)
#define GFX (SHIM_BASE + 0x03000U)
#define GFX_VT (SHIM_BASE + 0x04000U)
#define FileMgr (SHIM_BASE + 0x05000U)
#define FileMgr_VT (SHIM_BASE + 0x06000U)
/* FS_VT 是 FileMgr_VT 的别名（历史遗留命名，两者是同一张表） */
#define FS_VT FileMgr_VT
#define FILE1 (SHIM_BASE + 0x07000U)
#define FILE_VT (SHIM_BASE + 0x08000U)
#define AUDIO (SHIM_BASE + 0x09000U)
#define AUDIO_VT (SHIM_BASE + 0x0A000U)
#define AP (SHIM_BASE + 0x0B000U)
#define AP_VT (SHIM_BASE + 0x0C000U)
#define SVC04 (SHIM_BASE + 0x0D000U) /* 0x1000004 服务对象 */
#define SVC04_VT (SHIM_BASE + 0x0E000U)
#define SVC09 (SHIM_BASE + 0x0F000U) /* 0x1000009 服务对象 */
#define SVC09_VT (SHIM_BASE + 0x10000U)
#define CBK_OBJ (SHIM_BASE + 0x11000U)    /* root.createCallback 返回的对象 */
#define CBK_OBJ_VT (SHIM_BASE + 0x12000U) /* 可写：applet 会覆写 vt[+8] */
#define DLL_OBJ (SHIM_BASE + 0x13000U)    /* loadDLL 返回的 stub DLL 对象 */
#define DLL_OBJ_VT (SHIM_BASE + 0x14000U)
#define SVC_GENERIC (SHIM_BASE + 0x15000U) /* 未知服务号的兜底对象 */
#define SVC_GENERIC_VT (SHIM_BASE + 0x16000U)
#define IMAGE_VT (SHIM_BASE + 0x17000U) /* IImage 虚表（图片对象在堆上） */

/* -------------------- 纯数据槽（不是 trap，全部清零） -------------------- */
#define ZM_DATA_BASE (SHIM_BASE + 0x40000U)
#define ZM_DATA_SIZE 0x2000U
#define SIZE_SLOT (ZM_DATA_BASE + 0x000U) /* applet 写入所需堆大小 */
#define API_SLOT (ZM_DATA_BASE + 0x010U)  /* applet 写入 api 表，+8 是 handler */
#define DUMMY_BUF (ZM_DATA_BASE + 0x040U) /* spec_lookup 的一字节回写缓冲 */
#define INIT_CTX (ZM_DATA_BASE + 0x100U)  /* 1KB 零填充：init 事件 r3 上下文 */
#define SCRATCH_BUF (ZM_DATA_BASE + 0x800U) /* 通用临时缓冲（2KB） */

// -------------------- trap 地址宏 --------------------
/* SHIM 中的槽位地址 → 对应的 TRAMP 陷阱地址（一一对应） */
#define TRAP(slot_addr) (TRAMP_BASE + (slot_addr) - SHIM_BASE)
/* 反向：TRAMP 陷阱地址 → SHIM 槽位地址 */
#define UNTRAP(trap_addr) (SHIM_BASE + (trap_addr) - TRAMP_BASE)

/* -------------------- 控制用伪返回地址 --------------------
 * 放在 TRAMP 区末尾，保证不与任何真实 vtable 槽冲突。 */
#define TR_init_callback (TRAMP_BASE + TRAMP_SIZE - 0x10U)
#define TR_enter_event_loop (TRAMP_BASE + TRAMP_SIZE - 0x0CU)

/* -------------------- ROOT 函数表 trap -------------------- */
#define TR_root_queryRuntime TRAP(ROOT + 0x00U)
#define TR_root_get_ctx TRAP(ROOT + 0x04U)      /* 取上下文对象（00000440 sub_3BE8C），
                                                 * 返回可偏移访问的缓冲基址 */
#define TR_root_malloc TRAP(ROOT + 0x08U)
#define TR_root_free TRAP(ROOT + 0x0CU)
#define TR_root_18 TRAP(ROOT + 0x18U)   /* 00000001 sub_16C5C: 内存复制(dst,size,src) */
#define TR_root_str_copy TRAP(ROOT + 0x20U)
#define TR_root_alloc_big TRAP(ROOT + 0x30U)   /* 大块分配：r0=字节数 → 客户机指针 */
#define TR_root_free_big TRAP(ROOT + 0x34U)    /* 大块释放：r0=指针 */
#define TR_root_trace TRAP(ROOT + 0x3CU)       /* 调试输出（0000050b sub_2C8CC），no-op */
#define TR_root_get_foo TRAP(ROOT + 0x40U)     /* 未明确语义，00000001 用，stub 返 0 */
/* 标准 C 库槽位：偏移由 0000050c 的 ROOT thunk 表逐个反推确认
 * （见 zm_root.c 顶部注释）。 */
#define TR_root_wcstombs TRAP(ROOT + 0x24U)
#define TR_root_memcmp TRAP(ROOT + 0x50U)
#define TR_root_memmove TRAP(ROOT + 0x58U)
#define TR_root_memcpy TRAP(ROOT + 0x5CU)
#define TR_root_memset TRAP(ROOT + 0x60U)
#define TR_root_strtod TRAP(ROOT + 0x70U)
#define TR_root_strtol TRAP(ROOT + 0x74U)
#define TR_root_strlen TRAP(ROOT + 0x90U)
#define TR_root_94 TRAP(ROOT + 0x94U)          /* 00000001 sub_165DC：取路径上下文，stub 返 0 */
#define TR_root_98 TRAP(ROOT + 0x98U)          /* 00000001 sub_16604：写数据目录路径到缓冲，stub 返 0 */
#define TR_root_strstr TRAP(ROOT + 0xB0U)
#define TR_root_sprintf TRAP(ROOT + 0x6CU)
#define TR_root_str_assign TRAP(ROOT + 0x78U)
#define TR_root_str_ctor TRAP(ROOT + 0x88U)
#define TR_root_spec_lookup TRAP(ROOT + 0xA4U)
#define TR_root_str_find TRAP(ROOT + 0xA8U)
#define TR_root_get_tick TRAP(ROOT + 0xD8U)
#define TR_root_model_check TRAP(ROOT + 0x80U) /* 型号查询（00000405），返 0=默认布局 */
#define TR_root_prop_c8 TRAP(ROOT + 0xC8U)     /* 系统属性查询（00000405） */
#define TR_root_prop_d0 TRAP(ROOT + 0xD0U)     /* 备用 key 查询（00000405） */
#define TR_root_set_timer TRAP(ROOT + 0x148U)   /* IShell_SetTimer(ms, cb, param) */
#define TR_root_str_utf16 TRAP(ROOT + 0x140U)   /* 00000405 sub_84C6C: 复制 UTF-16 串 */
#define TR_root_get_status TRAP(ROOT + 0x144U)  /* 状态查询（0000050c sub_3790），no-op 返 0 */
#define TR_root_cancel_timer TRAP(ROOT + 0x14CU) /* IShell_CancelTimer(cb) */
#define TR_root_create_cbk TRAP(ROOT + 0x154U)

/* -------------------- runtime -------------------- */
#define TR_rt_release TRAP(RT_VT + 0x04U)
#define TR_rt_queryInterface TRAP(RT_VT + 0x08U)
#define TR_rt_getSystemInfo TRAP(RT_VT + 0x10U)
#define TR_rt_timer TRAP(RT_VT + 0x3CU)    /* 周期回调注册 (rt, ms, cb, param) */
#define TR_rt_get_appid TRAP(RT_VT + 0x2CU)  /* 取 applet 逻辑 ID（00000001 sub_103D0 用） */
#define TR_rt_getter TRAP(RT_VT + 0x48U)   /* 无参 getter，00000440 ×8 */
#define TR_rt_loadDLL TRAP(RT_VT + 0x58U)
#define TR_rt_unloadDLL TRAP(RT_VT + 0x5CU)
#define TR_rt_loadDLL2 TRAP(RT_VT + 0x78U)
#define TR_rt_40 TRAP(RT_VT + 0x40U)   /* 00000504 用，无明确语义，stub 返 0 */

/* -------------------- gfx（AEE_IDisplay）--------------------
 *
 * 槽位语义来自 00000405.app 里保留的调试字符串
 * （"ZmaeePhoneSearch_createlayer"、"AEE_IDisplay_SetActiveLayer"、
 *  "AEE_IDisplay_FreeAllLayer"、"AEE_IDisplay_DrawText"）
 * 以及 0000050b / 00000506 的直接帧缓冲用法交叉验证。
 */
#define TR_gfx_release TRAP(GFX_VT + 0x04U)
#define TR_gfx_create_layer TRAP(GFX_VT + 0x0CU)  /* createLayer(id, rect*) */
#define TR_gfx_vt10 TRAP(GFX_VT + 0x10U)           /* getFramebuffer(disp,flag,out*) 00000001 sub_7D68 */
#define TR_gfx_layer_info TRAP(GFX_VT + 0x1CU)    /* getLayerInfo(id, info*) */
#define TR_gfx_free_layers TRAP(GFX_VT + 0x18U)   /* freeAllLayer() */
#define TR_gfx_active_layer TRAP(GFX_VT + 0x20U)  /* setActiveLayer(id) */
#define TR_gfx_update_layer TRAP(GFX_VT + 0x28U)  /* updateLayer(id,x,y,w,h) */
#define TR_gfx_get_active_layer TRAP(GFX_VT + 0x30U) /* getActiveLayer() */
#define TR_gfx_fillRect TRAP(GFX_VT + 0x2CU)
#define TR_gfx_begin_paint TRAP(GFX_VT + 0x34U)
#define TR_gfx_end_paint TRAP(GFX_VT + 0x38U)
#define TR_gfx_vt3C TRAP(GFX_VT + 0x3CU)   /* 000004051/00000502 用，stub 返 0 */
#define TR_gfx_commit TRAP(GFX_VT + 0x40U)
#define TR_gfx_get_width TRAP(GFX_VT + 0x48U)
#define TR_gfx_measure_char TRAP(GFX_VT + 0x4CU)
#define TR_gfx_drawText TRAP(GFX_VT + 0x50U)
#define TR_gfx_clear_layer TRAP(GFX_VT + 0x54U) /* clearLayer(id, color) */
#define TR_gfx_fillRect5C TRAP(GFX_VT + 0x5CU)  /* fillRect(x,y,w,h)，00000440 ×5 */
#define TR_gfx_drawRect TRAP(GFX_VT + 0x6CU)
#define TR_gfx_fillRect2 TRAP(GFX_VT + 0x70U)
#define TR_gfx_vt44 TRAP(GFX_VT + 0x44U)   /* 无参 display 操作，00000405，stub */
#define TR_gfx_vt84 TRAP(GFX_VT + 0x84U)   /* 绘制辅助(x,y,w/2,...)，00000440 ×31，stub */
#define TR_gfx_draw_line TRAP(GFX_VT + 0x68U) /* DrawLine(x1,y1,x2,y2,color)，00000405 ×20 */
#define TR_gfx_vt8C TRAP(GFX_VT + 0x8CU)  /* drawWidgetBitmap 变体，stub */
#define TR_gfx_draw_image1 TRAP(GFX_VT + 0x90U) /* drawImage(x,y,img[,rect]) */
#define TR_gfx_draw_image2 TRAP(GFX_VT + 0x94U)
#define TR_gfx_draw_image98 TRAP(GFX_VT + 0x98U) /* drawImage 变体（00000506） */
#define TR_gfx_vtB0 TRAP(GFX_VT + 0xB0U)  /* 未明确语义，stub */
#define TR_gfx_vtB4 TRAP(GFX_VT + 0xB4U)  /* 三段式横条 blit (obj,xy,imgInfo,param) */
#define TR_gfx_image_file TRAP(GFX_VT + 0xA4U) /* createImageFromFile(...) */
#define TR_gfx_image_new TRAP(GFX_VT + 0xA8U)  /* createImage(alloc,free,out) */
#define TR_gfx_draw_image3 TRAP(GFX_VT + 0xACU)
#define TR_gfx_vtCC TRAP(GFX_VT + 0xCCU)    /* 000003e8/00000434/00000501/000005f9 共享，疑似 setRegion */
#define TR_gfx_vt1BC TRAP(GFX_VT + 0x1BCU)  /* 00000400 用，对象回调/方法，stub 返 0 */

/* -------------------- IImage -------------------- */
#define TR_img_release TRAP(IMAGE_VT + 0x04U)
#define TR_img_load_file TRAP(IMAGE_VT + 0x08U) /* load(flags, path, len) */
#define TR_img_get_size TRAP(IMAGE_VT + 0x10U)  /* getSize(&{w,h}) */
#define TR_img_ready TRAP(IMAGE_VT + 0x14U)
#define TR_img_make_desc TRAP(IMAGE_VT + 0x1CU) /* 生成可绘制描述符 */

/* -------------------- fs / file -------------------- */
#define TR_fileMgr_release TRAP(FileMgr_VT + 0x04U)
#define TR_fileMgr_open_file TRAP(FileMgr_VT + 0x08U)
#define TR_fileMgr_remove TRAP(FileMgr_VT + 0x0CU)
#define TR_fileMgr_rename TRAP(FileMgr_VT + 0x10U)
#define TR_fileMgr_mkdir TRAP(FileMgr_VT + 0x14U)
#define TR_fileMgr_rmdir TRAP(FileMgr_VT + 0x18U)
#define TR_fileMgr_exists TRAP(FileMgr_VT + 0x1CU)
#define TR_fileMgr_stat TRAP(FileMgr_VT + 0x20U) /* IsFile：常规文件→非0 */
#define TR_fileMgr_chdir TRAP(FileMgr_VT + 0x2CU)
#define TR_fileMgr_enum TRAP(FileMgr_VT + 0x30U)

#define TR_file_release TRAP(FILE_VT + 0x04U)
#define TR_file_read TRAP(FILE_VT + 0x08U)
#define TR_file_write TRAP(FILE_VT + 0x0CU)
#define TR_file_seek TRAP(FILE_VT + 0x20U)
/* 真机 AEE IFile 布局（经 0000050b sub_24664 / 00000440 sub_38830 确认）：
 *   0x20 = seek(whence, offset)，whence 枚举 0=SET / 1=END
 *   0x24 = tell()    —— 返回当前读写位置
 *   0x28 = size()    —— 返回文件大小
 * 早期实现把 0x24 当 size、0x28 当 tell 且 whence 1=CUR，导致 seek(1,0)
 * 不能跳到末尾，取 size 的惯用法（tell→seek(1,0)→tell→seek(0,old)）失效。 */
#define TR_file_tell TRAP(FILE_VT + 0x24U)
#define TR_file_size TRAP(FILE_VT + 0x28U)

/* -------------------- audio -------------------- */
#define TR_audio_release TRAP(AUDIO_VT + 0x04U)
#define TR_audio_stop TRAP(AUDIO_VT + 0x14U)
#define TR_audio_vt18 TRAP(AUDIO_VT + 0x18U) /* SetEnable(this, enable) */
#define TR_audio_vt1C TRAP(AUDIO_VT + 0x1CU) /* GetPlayStatus(this,&o1,&o2,&o3) */
#define TR_audio_vt20 TRAP(AUDIO_VT + 0x20U) /* SetDelay/SetVolume(this, val) */
#define TR_audio_get_status TRAP(AUDIO_VT + 0x24U)
#define TR_ap_release TRAP(AP_VT + 0x04U)
#define TR_ap_play TRAP(AP_VT + 0x10U)
#define TR_ap_stop TRAP(AP_VT + 0x14U)

/* -------------------- 服务对象 / DLL / 回调 -------------------- */
#define TR_svc04_release TRAP(SVC04_VT + 0x04U)
#define TR_svc04_x1C TRAP(SVC04_VT + 0x1CU)
#define TR_svc09_release TRAP(SVC09_VT + 0x04U)
#define TR_svc09_x2C TRAP(SVC09_VT + 0x2CU)
#define TR_svc09_x40 TRAP(SVC09_VT + 0x40U)
#define TR_svcg_release TRAP(SVC_GENERIC_VT + 0x04U)
#define TR_dll_release TRAP(DLL_OBJ_VT + 0x04U)
#define TR_dll_init TRAP(DLL_OBJ_VT + 0x08U)
#define TR_dll_config TRAP(DLL_OBJ_VT + 0x0CU)
#define TR_dll_entry TRAP(DLL_OBJ_VT + 0x10U)
#define TR_cbk_release TRAP(CBK_OBJ_VT + 0x04U)
#define TR_cbk_default TRAP(CBK_OBJ_VT + 0x08U)

// -------------------- 全局变量 --------------------
extern uc_engine *g_uc;
extern AppletHeader g_header;
extern uint32_t g_heap_ptr;
extern uint32_t g_vram_ptr;

extern uint32_t g_instance;
extern uint32_t g_handler;
extern int g_trap_pause;
extern int g_disasm;

/* 运行控制（由 main.c 的命令行解析填充） */
extern int g_headless;           /* 1=无窗口（SDL dummy 驱动） */
extern uint32_t g_hold_ms;       /* 事件循环每轮最长停留毫秒，0=直到关窗 */
extern uint32_t g_max_events;    /* 最多派发多少轮事件后结束，0=不限 */
extern uint32_t g_event_rounds;  /* 已经跑过的事件循环轮数 */
extern uint32_t g_unknown_traps; /* 命中未实现外部调用的次数 */
extern int g_stop_requested;     /* 请求结束模拟（窗口关闭 / 达到上限） */

/* 当前载入 applet 的完整路径，由 main.c 设置。
 * TR_init_callback 取其中的短名写入 applet instance+4。 */
extern char g_app_pathname[4096];

// 用来反汇编用的一组全局变量，之所以是全局变量是因为要不停的复用
extern csh g_cs_handle;
extern cs_insn *g_sc_insn;
extern size_t g_sc_count;
extern uint8_t g_cscode[16];

// -------------------- 函数声明 --------------------

/* 构建所有虚表：把 trap 地址写入客户机虚拟内存中的 shim 区 */
int zm_emu_build_vtables(void);

/* Unicorn 内存映射（blob/stack/heap/shim/tramp） */
int zm_emu_map_memory(void);

/* 载入 applet blob 到 BLOB_BASE */
int zm_emu_load_blob(FILE *fp, const long *applet_size);

/* 注册 Unicorn 钩子（code / unmapped mem / shim mem） */
int zm_emu_add_hooks(void);

/* 设置初始寄存器，启动 applet；成功返回 0 */
int zm_emu_start_applet(void);

/* 把一个 TRAMP 陷阱地址翻译成 "对象名[偏移]" 形式，便于阅读日志。
 * 返回内部静态缓冲区，非线程安全。 */
const char *zm_trap_name(uint32_t trap_address);

/* 按需映射包含 address 的一页客户机内存（未映射访问兜底） */
bool zm_emu_map_on_demand(uint64_t address);

/* 在客户机堆上分配并写入一段数据，返回客户机地址（失败返回 0） */
uint32_t zm_emu_alloc_guest(const void *data, uint32_t len);

/* 在显存区分配像素缓冲（图层 / 图片），返回客户机地址（失败返回 0）。
 * 与 applet 的堆完全隔离，保证渲染不会被 applet 的大块分配挤掉。 */
uint32_t zm_emu_alloc_vram(const void *data, uint32_t len);

#endif

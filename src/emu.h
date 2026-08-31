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
#define SHELL_VT (SHIM_BASE + 0x180U) /* g_aee_shell_vtbl @ .data:0x64440，34 槽 */
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
/* IShell.CreateInstance 返回的服务对象（RE 实测 CLSID：16777220=INetMgr、
 * 16777225=ITAPI；旧名 SVC04/SVC09 为误命名） */
#define NETMGR (SHIM_BASE + 0x900U) /* 0x1000004 INetMgr 服务对象 */
#define NETMGR_VT (SHIM_BASE + 0x910U)
#define TAPI (SHIM_BASE + 0x940U) /* 0x1000009 ITAPI 服务对象 */
#define TAPI_VT (SHIM_BASE + 0x950U)
#define CBK_OBJ (SHIM_BASE + 0x980U)    /* sub_84E04 返回的回调对象 */
#define CBK_OBJ_VT (SHIM_BASE + 0x990U) /* 可写：applet 覆写 vt[+8] */
#define DLL_OBJ (SHIM_BASE + 0x9C0U)    /* loadDLL 返回的 stub DLL 对象 */
#define DLL_OBJ_VT (SHIM_BASE + 0x9D0U)

/* ---- ZMAEE IDisplay / IBitmap 原生虚表（逆向实测 g_aee_display_vtbl /
 * g_aee_bitmap_vtbl @ .data:0x63E10 / 0x63DF4）----
 * display 是全局单例，由 queryInterface(0x1000005) 返回。旧 GFX/GFX_VT
 * 是早期对同一张表的误命名（实测偏移与本表吻合），已并入此处。
 * bitmap 由 IDisplay.CreateBitmap/LoadBitmap 创建，这里用单个 BITMAP 单例
 * 作为所有 bitmap 对象的 vtable 模板（真实多实例后续再扩展）。 */
#define DISPLAY (SHIM_BASE + 0xA00U)    /* 全局 display 对象（0x1000005） */
#define DISPLAY_VT (SHIM_BASE + 0xA80U) /* 58 槽 ×4B = 0xE8 */
#define BITMAP (SHIM_BASE + 0xB80U)     /* bitmap 单例对象 */
#define BITMAP_VT (SHIM_BASE + 0xC00U)  /* 7 槽 ×4B = 0x1C */

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
/* ---- ZMAEE IShell 原生虚表（g_aee_shell_vtbl @ .data:0x64440，34 槽）----
 * +0x08 CreateInstance（旧称 queryInterface）、+0x10 GetDeviceInfo（旧称
 * getSystemInfo）、+0x58 LoadDLL（RE sub_35230）、+0x5C UnloadDLL
 * （RE sub_346D8）、+0x78 LoadLibraryExt（旧称 loadDLL2）此前已按行为
 * 实现；其余槽接 zm_shell_stub，保证不落 "非法的外部调用"。 */
#define TR_shell_AddRef TRAP(SHELL_VT + 0x00U)
#define TR_shell_Release TRAP(SHELL_VT + 0x04U)
#define TR_shell_CreateInstance TRAP(SHELL_VT + 0x08U)
#define TR_shell_x0C TRAP(SHELL_VT + 0x0CU) /* RE sub_34DE4，未知 */
#define TR_shell_GetDeviceInfo TRAP(SHELL_VT + 0x10U)
#define TR_shell_GetRootDir TRAP(SHELL_VT + 0x14U)
#define TR_shell_SetWorkDir TRAP(SHELL_VT + 0x18U)
#define TR_shell_GetWorkDir TRAP(SHELL_VT + 0x1CU)
#define TR_shell_StartApplet TRAP(SHELL_VT + 0x20U)
#define TR_shell_x24 TRAP(SHELL_VT + 0x24U) /* RE sub_3482C，未知 */
#define TR_shell_CanStartApplet TRAP(SHELL_VT + 0x28U)
#define TR_shell_ActiveApplet TRAP(SHELL_VT + 0x2CU)
#define TR_shell_GetApplet TRAP(SHELL_VT + 0x30U)
#define TR_shell_x34 TRAP(SHELL_VT + 0x34U) /* RE sub_34764，未知 */
#define TR_shell_x38 TRAP(SHELL_VT + 0x38U) /* RE sub_34C1C，未知 */
#define TR_shell_SetTimer TRAP(SHELL_VT + 0x3CU)
#define TR_shell_CancelTimer TRAP(SHELL_VT + 0x40U)
#define TR_shell_CancelOwnerTimer TRAP(SHELL_VT + 0x44U)
#define TR_shell_GetTickCount TRAP(SHELL_VT + 0x48U)
#define TR_shell_OpenWapBrowser TRAP(SHELL_VT + 0x4CU)
#define TR_shell_x50 TRAP(SHELL_VT + 0x50U) /* RE sub_3474C，未知 */
#define TR_shell_SetEndKeyMask TRAP(SHELL_VT + 0x54U)
#define TR_shell_LoadDLL TRAP(SHELL_VT + 0x58U)
#define TR_shell_UnloadDLL TRAP(SHELL_VT + 0x5CU)
#define TR_shell_GetAppletMask TRAP(SHELL_VT + 0x60U)
#define TR_shell_SetAppletMask TRAP(SHELL_VT + 0x64U)
#define TR_shell_IsLoadGlobalLibrary TRAP(SHELL_VT + 0x68U)
#define TR_shell_LoadGlobalLibrary TRAP(SHELL_VT + 0x6CU)
#define TR_shell_FreeGlobalLibrary TRAP(SHELL_VT + 0x70U)
#define TR_shell_IsGlobalLibraryUseStaticMem TRAP(SHELL_VT + 0x74U)
#define TR_shell_LoadLibraryExt TRAP(SHELL_VT + 0x78U)
#define TR_shell_EntryApplet TRAP(SHELL_VT + 0x7CU)
#define TR_shell_GetAppDir TRAP(SHELL_VT + 0x80U)
#define TR_shell_GetSupportHall TRAP(SHELL_VT + 0x84U)
// fs
/*
 * IFile 虚表（逆向实测：gAEEFileVtbl @ .data:00064010，函数指针均 +1 表示 Thumb）
 *
 *   +0x00  ZMAEE_IFile_AddRef
 *   +0x04  ZMAEE_IFile_Release   ← 本模拟器的 file.close
 *   +0x08  ZMAEE_IFile_Read      ← 本模拟器的 file.read
 *   +0x0C  ZMAEE_IFile_Write     （未接线）
 *   +0x10  ZMAEE_IFile_Readable  （未接线）
 *   +0x14  ZMAEE_IFile_Writeable （未接线）
 *   +0x18  ZMAEE_IFile_Cancel    （未接线）
 *   +0x1C  ZMAEE_IFile_Flush     （未接线）
 *   +0x20  ZMAEE_IFile_Seek      ← 本模拟器的 file.seek
 *   +0x24  ZMAEE_IFile_Tell      ← 本模拟器的 file.tell
 *
 * 关键点：表里**没有** GetSize/Size 槽位。取文件大小的惯用法只能是
 *   Seek(0, SEEK_END) 然后 Tell()（此时位置恰好等于总大小）。
 * 因此 +0x24 必须实现为 Tell（返回当前读写位置）。
 * 之前实现成"返回总大小"是错的 —— 只在"先 seek 到末尾"这一种调用序列下
 * 碰巧正确，在任意位置调用会给出错误结果。
 *
 * 注：seek 的参数序（whence/offset 谁在前）尚无 applet 覆盖验证，
 * 保持现状未改动；若后续有 applet 用到 seek，需用 RE 数据核对。
 */
#define TR_fileMgr_open_file TRAP(FileMgr_VT + 0x08U)
#define TR_file_close TRAP(FILE_VT + 0x04U) /* Release */
#define TR_file_read TRAP(FILE_VT + 0x08U)
#define TR_file_seek TRAP(FILE_VT + 0x20U)
#define TR_file_tell TRAP(FILE_VT + 0x24U) /* Tell：返回当前读写位置 */
// audio

#define TR_audio_stop TRAP(AUDIO_VT + 0x14U)
#define TR_ap_play TRAP(AP_VT + 0x10U)
#define TR_ap_stop TRAP(AP_VT + 0x14U)
#define TR_audio_get_status TRAP(AUDIO_VT + 0x24U) /* AUDIO_VT[0x24] */

/* 00000405.app：FS vtable 缺失槽 */
#define TR_fs_enum TRAP(FS_VT + 0x30U) /* FS_VT[0x30]：enumFile */

/* 00000405.app 新增 ROOT vtable trap（索引 23..45） */

/*
 * 实测修正：applet 00000440 实际跳转 0x8A0060，即 ROOT+0x60；
 * 原先写成 ROOT+0x28 导致该 case 永不命中（memset 落到 default 分支）。
 * 注意这一段的偏移与注释普遍对不上，其他条目待逐个用真实 applet 验证。
 */
#define TR_root_memset TRAP(ROOT + 0x60U)
#define TR_root_x74 TRAP(ROOT + 0x29U)
#define TR_root_str_assign TRAP(ROOT + 0x30U) /* ROOT[0x78] */
#define TR_root_get_tick TRAP(ROOT + 0xD8U)   /* ROOT[0xD8] */
#define TR_root_x12C TRAP(ROOT + 0x12CU)
#define TR_root_x130 TRAP(ROOT + 0x130U)
#define TR_root_x140 TRAP(ROOT + 0x140U)
#define TR_root_create_cbk TRAP(ROOT + 0x154U)
#define TR_root_x16C TRAP(ROOT + 0x16CU)

/* ---- 服务对象 / FS / DLL / CBK trap ----
 * 旧「索引 46..57」方案把这些宏挂在 ROOT+0x2E..0x3F 的伪造索引上，与
 * SHIM↔TRAMP 对射派发不符：applet 经对象虚表发起的真实调用落在各自
 * VT 槽位，伪造索引永不命中（与已修的 loadDLL 同病）。现按真实槽位挂接。
 * fs_chdir 的真实槽位未知，暂缺（其 case 已删，待 RE 后补）。 */
#define TR_netmgr_release TRAP(NETMGR_VT + 0x04U)
#define TR_netmgr_x1C TRAP(NETMGR_VT + 0x1CU)
#define TR_tapi_release TRAP(TAPI_VT + 0x04U)
#define TR_tapi_x2C TRAP(TAPI_VT + 0x2CU)
#define TR_tapi_x40 TRAP(TAPI_VT + 0x40U)
#define TR_fs_release TRAP(FileMgr_VT + 0x04U)
#define TR_dll_init TRAP(DLL_OBJ_VT + 0x08U)
#define TR_dll_config TRAP(DLL_OBJ_VT + 0x0CU)
#define TR_dll_entry TRAP(DLL_OBJ_VT + 0x10U)
#define TR_cbk_default TRAP(CBK_OBJ_VT + 0x08U)

/* ---- ZMAEE IDisplay 原生虚表（g_aee_display_vtbl @ .data:0x63E10）----
 * 偏移严格按逆向贴出的表。带「实测」的槽为旧 GFX 路径验证过的行为
 * （gfx 与 display 本是同一张表），其余为按槽序推测命名，语义待 RE 校准。
 * DISPLAY_VT 58 槽止于 +0xE4，更远的偏移（如旧 GFX 路径见过的 +0x114）
 * 不在本表内，属其它对象/越界调用。 */
#define TR_display_AddRef TRAP(DISPLAY_VT + 0x00U)
#define TR_display_Release TRAP(DISPLAY_VT + 0x04U)
#define TR_display_GetMaxLayerCount TRAP(DISPLAY_VT + 0x08U)
#define TR_display_CreateLayer TRAP(DISPLAY_VT + 0x0CU)
#define TR_display_CreateLayerExt TRAP(DISPLAY_VT + 0x10U)
#define TR_display_FreeLayer TRAP(DISPLAY_VT + 0x14U)
#define TR_display_x18 TRAP(DISPLAY_VT + 0x18U) /* 实测被调，功能未知 stub */
#define TR_display_GetLayerInfo TRAP(DISPLAY_VT + 0x1CU)
#define TR_display_clear TRAP(DISPLAY_VT + 0x20U) /* 实测：clear(color) */
#define TR_display_SetLayerPosition TRAP(DISPLAY_VT + 0x24U)
#define TR_display_Update TRAP(DISPLAY_VT + 0x28U)
#define TR_display_fillRectR TRAP(DISPLAY_VT + 0x2CU) /* 实测：fillRect(rect_ptr)，空实现疑似 invalidate */
#define TR_display_GetActiveLayer TRAP(DISPLAY_VT + 0x30U)
#define TR_display_x34 TRAP(DISPLAY_VT + 0x34U) /* 实测被调，功能未知 stub */
#define TR_display_UnlockScreen TRAP(DISPLAY_VT + 0x38U)
#define TR_display_RegisterCustomFont TRAP(DISPLAY_VT + 0x3CU)
#define TR_display_commit TRAP(DISPLAY_VT + 0x40U) /* 实测：commit 提交帧缓冲 */
#define TR_display_GetFontWidth TRAP(DISPLAY_VT + 0x44U)
#define TR_display_getWidth TRAP(DISPLAY_VT + 0x48U) /* 实测：返回屏幕宽度 */
#define TR_display_measureChar TRAP(DISPLAY_VT + 0x4CU) /* 实测：measureChar(gfx, char_ptr, count, width_out, sp[metrics]) */
#define TR_display_DrawText TRAP(DISPLAY_VT + 0x50U) /* 实测吻合：drawText */
#define TR_display_SetTransColor TRAP(DISPLAY_VT + 0x54U)
#define TR_display_SetOpacity TRAP(DISPLAY_VT + 0x58U)
#define TR_display_SetClipRect TRAP(DISPLAY_VT + 0x5CU)
#define TR_display_GetClipRect TRAP(DISPLAY_VT + 0x60U)
#define TR_display_SetPixel TRAP(DISPLAY_VT + 0x64U)
#define TR_display_DrawLine TRAP(DISPLAY_VT + 0x68U)
#define TR_display_DrawRect TRAP(DISPLAY_VT + 0x6CU)   /* 实测吻合：drawRect */
#define TR_display_FillRect TRAP(DISPLAY_VT + 0x70U)   /* 实测吻合：fillRect */
#define TR_display_DrawRoundRect TRAP(DISPLAY_VT + 0x74U)
#define TR_display_DrawCircle TRAP(DISPLAY_VT + 0x78U)
#define TR_display_FillCircle TRAP(DISPLAY_VT + 0x7CU)
#define TR_display_DrawArc TRAP(DISPLAY_VT + 0x80U)
#define TR_display_FillArc TRAP(DISPLAY_VT + 0x84U)
#define TR_display_FillGradientRect TRAP(DISPLAY_VT + 0x88U)
#define TR_display_AlphaBlendRect TRAP(DISPLAY_VT + 0x8CU)
#define TR_display_DrawImage TRAP(DISPLAY_VT + 0x90U)
#define TR_display_DrawBitmap TRAP(DISPLAY_VT + 0x94U)
#define TR_display_DrawBitmapEx TRAP(DISPLAY_VT + 0x98U)
#define TR_display_DrawBitmapFrame TRAP(DISPLAY_VT + 0x9CU)
#define TR_display_CreateBitmap TRAP(DISPLAY_VT + 0xA0U) /* 返回 BITMAP 单例 */
#define TR_display_LoadBitmap TRAP(DISPLAY_VT + 0xA4U)   /* 返回 BITMAP 单例 */
#define TR_display_CreateImage TRAP(DISPLAY_VT + 0xA8U)
#define TR_display_BitBlt TRAP(DISPLAY_VT + 0xACU)
#define TR_display_Flatten TRAP(DISPLAY_VT + 0xB0U)
#define TR_display_StretchBlt TRAP(DISPLAY_VT + 0xB4U)
#define TR_display_DrawAntialiasingLine TRAP(DISPLAY_VT + 0xB8U)
#define TR_display_DrawWLine TRAP(DISPLAY_VT + 0xBCU)
#define TR_display_GetDMLayerHdlr TRAP(DISPLAY_VT + 0xC0U)
#define TR_display_RelevanceLayer TRAP(DISPLAY_VT + 0xC4U)
#define TR_display_Refresh TRAP(DISPLAY_VT + 0xC8U)
#define TR_display_DrawImageExt TRAP(DISPLAY_VT + 0xCCU)
#define TR_display_DrawSysWallPaper TRAP(DISPLAY_VT + 0xD0U)
#define TR_display_DrawBorderText TRAP(DISPLAY_VT + 0xD4U)
#define TR_display_PushAndSetAlphaLayer TRAP(DISPLAY_VT + 0xD8U)
#define TR_display_PopAndRestoreAlphaLayer TRAP(DISPLAY_VT + 0xDCU)
#define TR_display_RotateScreen TRAP(DISPLAY_VT + 0xE0U)

/* ---- ZMAEE IBitmap 原生虚表（g_aee_bitmap_vtbl @ .data:0x63DF4）----
 * +0x0C/0x14/0x18 是 sub_25F78/sub_25F84/sub_25FF8（未知），接 stub。 */
#define TR_bitmap_AddRef TRAP(BITMAP_VT + 0x00U)
#define TR_bitmap_Release TRAP(BITMAP_VT + 0x04U)
#define TR_bitmap_SetTransColor TRAP(BITMAP_VT + 0x08U)
#define TR_bitmap_sub_25F78 TRAP(BITMAP_VT + 0x0CU)
#define TR_bitmap_GetInfo TRAP(BITMAP_VT + 0x10U)
#define TR_bitmap_sub_25F84 TRAP(BITMAP_VT + 0x14U)
#define TR_bitmap_sub_25FF8 TRAP(BITMAP_VT + 0x18U)

//
#define TR_init_callback                                                       \
  TRAP(ROOT + 0x118cU) // 随便写一个位置我想也应该不影响这个叫什么来.初始化回调

// 事件回调因为 apple 是没有主循环的所以要用外部来完成这个主循环
#define TR_enter_event_loop TRAP(ROOT + 0x1180U)

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
#ifndef TRAP_INTERNAL_H
#define TRAP_INTERNAL_H

/**
 * @file trap_internal.h
 * @brief trap 子系统的**内部契约**（对外只有 trap.h 的 handle_trap/getArg）
 *
 * 这里只放三样东西：
 *   1. 现场结构 trap_ctx 与统一 handler 签名 trap_fn；
 *   2. 分派表项 trap_entry（槽区间 + fn + 字面量 off + kind）；
 *   3. 各 handler / 各辅助模块的**声明**（定义散在 trap_*.c 里）。
 *
 * 设计要点（与旧版 trap.c 的区别）：
 *   · 旧版用 AD_R0/AD_R1/AD_R01/AD_R012/AD_R0123/AD_0/AD_OFF 七个宏去适配
 *     各不相同的实函数签名；现在所有 handler 统一 (trap_ctx*)，寄存器从
 *     c->r0..c->sp 取，代价只是多写一个 c->uc，换来表里 fn 类型统一。
 *   · 槽的字面量偏移（display 的 0x5C、shell 的 0x18 …）从**宏参数**搬到
 *     **表项的 off 字段**，由分派器填进 c->off，handler 里不再有魔法数。
 *   · "handler 自己写 PC" 不再是 ctx 里的 handled 标志，而是表项的 kind
 *     列（TK_MANUAL_PC）——读表就能看出哪些槽是控制流。
 */

#include <stdbool.h>
#include <stdint.h>

#include "../emu.h"	 /* uc_engine / TRAMP_BASE / TR_* 槽位常量 / g_* 全局 */
#include "../trap.h" /* getArg（display/image 读第 5 个及以后的参数） */

/* -------------------- 现场 -------------------- */

typedef struct {
	uc_engine *uc;
	uint32_t trap; /* 陷阱地址 */
	uint32_t r0, r1, r2, r3;
	uint32_t lr, sp;
	uint32_t off; /* 槽的字面量偏移，由分派器从表项填入；无字面量的槽恒为 0 */
} trap_ctx;

typedef uint32_t (*trap_fn)(trap_ctx *c);

/* -------------------- 分派表 -------------------- */

/**
 * @brief handler 是否接管控制流
 *
 * 旧版是 handler 自己置 c->handled——跨层约定，读表的人看不出哪些槽改控制流。
 * 现在它是表的一列：看表就知道。
 */
enum trap_kind {
	TK_CALL = 0,  /**< 普通调用：分发器回写 R0，并把 PC 置为 LR */
	TK_MANUAL_PC, /**< handler 已自行写 PC / 停 emu：分发器不再动 R0/PC */
};

typedef struct {
	uint32_t lo, hi; /**< 槽地址区间（精确项 lo == hi） */
	trap_fn fn;
	uint32_t off; /**< 填进 ctx.off（无字面量的槽填 0） */
	uint8_t kind; /**< enum trap_kind */
} trap_entry;

/* 表项缩写：只做字段填充，不做签名适配（签名适配已由 trap_fn 统一）
 * 精确项 / 带 off 的精确项 / 控制流项 / 区间项 四种。 */
#define SLOT(trap, fn) {(trap), (trap), (fn), 0, TK_CALL}
#define SLOT_OFF(trap, fn, o) {(trap), (trap), (fn), (o), TK_CALL}
#define SLOT_PC(trap, fn) {(trap), (trap), (fn), 0, TK_MANUAL_PC}
#define RANGE(lo, hi, fn) {(lo), (hi), (fn), 0, TK_CALL}

/* -------------------- 客户机堆 / libc 接线（trap_cbk_heap.c）-------------------- */

uint32_t applet_malloc(uc_engine *uc, uint32_t size);
void applet_free(uc_engine *uc, uint32_t p);
uint32_t applet_calloc(uc_engine *uc, uint32_t n, uint32_t size);
void cbk_heap_init_once(uc_engine *uc);

/* -------------------- 调试基础设施（trap_debug.c）-------------------- */

/* 进入 handler 前：参数/寄存器 dump（ZM_DISASM）+ 统计探针 + 上下文镜像。
 * hook_ctx_apply 不是"调试"，但它必须在 handler 之前跑，故一并收在这里。 */
void trap_debug_before(
	uc_engine *uc, uint32_t trap, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t lr);
/* handler 返回后：回填环形缓冲的返回值（ring < 0 表示未记录） */
void trap_debug_after(int ring, uint32_t ret);
/* ZM_CB_PROBE：找 applet 把回调交给了哪个槽 */
void trap_debug_probe(
	uint32_t trap, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t lr);
/* 记一笔"applet → 外部"调用，返回环形缓冲下标（供 trap_debug_after 回填） */
int trap_ring_record(
	uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t lr);

/* -------------------- handlers：root 表（trap_handlers_root.c）-------------------- */

/* 控制流类（kind = TK_MANUAL_PC） */
uint32_t a_reg_callback(trap_ctx *c);
uint32_t a_enter_event_loop(trap_ctx *c);
uint32_t a_abort(trap_ctx *c);
uint32_t a_timer_return(trap_ctx *c);

uint32_t a_register_event_loop(trap_ctx *c);

uint32_t a_root_getShell(trap_ctx *c);
uint32_t a_root_malloc(trap_ctx *c);
uint32_t a_root_free(trap_ctx *c);
uint32_t a_root_malloc_screen(trap_ctx *c);
uint32_t a_root_free_screen(trap_ctx *c);
uint32_t a_root_x3C(trap_ctx *c);
uint32_t a_root_sprintf(trap_ctx *c);
uint32_t a_root_str_to_num(trap_ctx *c);
uint32_t a_root_atof(trap_ctx *c);
uint32_t a_root_f_op(trap_ctx *c);
uint32_t a_root_str_ctor(trap_ctx *c);
uint32_t a_root_srand(trap_ctx *c);
uint32_t a_root_rand(trap_ctx *c);
uint32_t a_root_sqrt(trap_ctx *c);
uint32_t a_root_cos(trap_ctx *c);
uint32_t a_root_sin(trap_ctx *c);
uint32_t a_root_atan(trap_ctx *c);
uint32_t a_root_tan(trap_ctx *c);
uint32_t a_root_create_cbk(trap_ctx *c);

/* root 表里的字符串 / 内存 / CBK 槽 */
uint32_t a_zm_root_x68C(trap_ctx *c);
uint32_t a_zm_ucs2_to_utf8(trap_ctx *c);
uint32_t a_zm_utf8_to_ucs2(trap_ctx *c);
uint32_t a_zm_root_str_assign(trap_ctx *c);
uint32_t a_zm_strcmp(trap_ctx *c);
uint32_t a_zm_strstr(trap_ctx *c);
uint32_t a_zm_strchr(trap_ctx *c);
uint32_t a_zm_spec_lookup(trap_ctx *c);
uint32_t a_zm_strlen(trap_ctx *c);
uint32_t a_zm_wcslen(trap_ctx *c);
uint32_t a_u_memcmp(trap_ctx *c);
uint32_t a_u_memcpy(trap_ctx *c);
uint32_t a_u_memset(trap_ctx *c);
uint32_t a_zm_root_cbk_default(trap_ctx *c);

/* -------------------- handlers：图形（trap_handlers_gfx.c）-------------------- */

uint32_t a_zm_display_AddRef(trap_ctx *c);
uint32_t a_zm_display_Release(trap_ctx *c);
uint32_t a_zm_display_GetMaxLayerCount(trap_ctx *c);
uint32_t a_zm_display_FreeAllLayer(trap_ctx *c);
uint32_t a_zm_display_GetActiveLayer(trap_ctx *c);
uint32_t a_zm_display_UnlockScreen(trap_ctx *c);
uint32_t a_zm_display_GetFontHeight(trap_ctx *c);
uint32_t a_zm_display_FreeLayer(trap_ctx *c);
uint32_t a_zm_display_SetActiveLayer(trap_ctx *c);
uint32_t a_zm_display_SelectFont(trap_ctx *c);
uint32_t a_zm_display_GetFontWidth(trap_ctx *c);
uint32_t a_zm_display_LoadBitmap(trap_ctx *c);
uint32_t a_zm_display_SetTransColor(trap_ctx *c);
uint32_t a_zm_display_Refresh(trap_ctx *c);
uint32_t a_zm_display_UpdateEx(trap_ctx *c);
uint32_t a_zm_display_CreateBitmap(trap_ctx *c);
uint32_t a_display_Update(trap_ctx *c);
uint32_t a_display_MeasureString(trap_ctx *c);
uint32_t a_display_DrawText(trap_ctx *c);
uint32_t a_display_DrawRect(trap_ctx *c);
uint32_t a_display_FillRect(trap_ctx *c);
uint32_t a_display_DrawBitmapEx(trap_ctx *c);
uint32_t a_display_BitBlt(trap_ctx *c);
uint32_t a_display_StretchBlt(trap_ctx *c);
/* 带字面量 off 的 display 槽：off 在表项里，见 trap_dispatch.c */
uint32_t a_disp_CreateLayer(trap_ctx *c);
uint32_t a_disp_CreateLayerExt(trap_ctx *c);
uint32_t a_disp_GetLayerInfo(trap_ctx *c);
uint32_t a_disp_SetLayerPosition(trap_ctx *c);
uint32_t a_disp_LockScreen(trap_ctx *c);
uint32_t a_disp_RegisterCustomFont(trap_ctx *c);
uint32_t a_disp_SetOpacity(trap_ctx *c);
uint32_t a_disp_SetClipRect(trap_ctx *c);
uint32_t a_disp_GetClipRect(trap_ctx *c);
uint32_t a_disp_SetPixel(trap_ctx *c);
uint32_t a_disp_DrawLine(trap_ctx *c);
uint32_t a_disp_DrawRoundRect(trap_ctx *c);
uint32_t a_disp_DrawCircle(trap_ctx *c);
uint32_t a_disp_FillCircle(trap_ctx *c);
uint32_t a_disp_DrawArc(trap_ctx *c);
uint32_t a_disp_FillArc(trap_ctx *c);
uint32_t a_disp_FillGradientRect(trap_ctx *c);
uint32_t a_disp_AlphaBlendRect(trap_ctx *c);
uint32_t a_disp_DrawImage(trap_ctx *c);
uint32_t a_disp_DrawBitmap(trap_ctx *c);
uint32_t a_disp_DrawBitmapFrame(trap_ctx *c);
uint32_t a_disp_Flatten(trap_ctx *c);
uint32_t a_disp_CreateImage(trap_ctx *c);
uint32_t a_disp_DrawAntialiasingLine(trap_ctx *c);
uint32_t a_disp_DrawWLine(trap_ctx *c);
uint32_t a_disp_GetDMLayerHdlr(trap_ctx *c);
uint32_t a_disp_RelevanceLayer(trap_ctx *c);
uint32_t a_disp_DrawImageExt(trap_ctx *c);
uint32_t a_disp_DrawSysWallPaper(trap_ctx *c);
uint32_t a_disp_DrawBorderText(trap_ctx *c);
uint32_t a_disp_PushAndSetAlphaLayer(trap_ctx *c);
uint32_t a_disp_PopAndRestoreAlphaLayer(trap_ctx *c);
uint32_t a_disp_RotateScreen(trap_ctx *c);

/* surface（区间 nop 见 d_surf_nop） */
uint32_t a_zm_surf_release(trap_ctx *c);
uint32_t a_zm_surf_getrect(trap_ctx *c);
uint32_t d_surf_nop(trap_ctx *c);

/* image */
uint32_t a_zm_image_AddRef(trap_ctx *c);
uint32_t a_zm_image_Release(trap_ctx *c);
uint32_t a_zm_image_GetFrameCount(trap_ctx *c);
uint32_t a_zm_image_Width(trap_ctx *c);
uint32_t a_zm_image_Height(trap_ctx *c);
uint32_t a_zm_image_GetType(trap_ctx *c);
uint32_t a_zm_image_SetData(trap_ctx *c);
uint32_t a_image_Decode(trap_ctx *c);
uint32_t d_image_stub(trap_ctx *c);

/* bitmap */
uint32_t a_zm_bitmap_AddRef(trap_ctx *c);
uint32_t a_zm_bitmap_Release(trap_ctx *c);
uint32_t a_zm_bitmap_SetTransColor(trap_ctx *c);
uint32_t a_zm_bitmap_GetInfo(trap_ctx *c);
uint32_t a_bmp_25F78(trap_ctx *c);
uint32_t a_bmp_25F84(trap_ctx *c);
uint32_t a_bmp_25FF8(trap_ctx *c);

/* -------------------- handlers：文件（trap_handlers_io.c）-------------------- */

uint32_t a_zm_file_close(trap_ctx *c);
uint32_t a_zm_file_read(trap_ctx *c);
uint32_t a_zm_file_write(trap_ctx *c);
uint32_t a_zm_file_seek(trap_ctx *c);
uint32_t a_zm_file_tell(trap_ctx *c);
uint32_t a_zm_fileMgr_GetFreeSize(trap_ctx *c);
uint32_t a_zm_fileMgr_GetInfo(trap_ctx *c);
uint32_t a_zm_fileMgr_TestFile(trap_ctx *c);
uint32_t a_zm_fileMgr_StorageSupport(trap_ctx *c);
uint32_t a_zm_fileMgr_make_dir(trap_ctx *c);
uint32_t a_fileMgr_open_file(trap_ctx *c);
uint32_t a_fileMgr_x3C(trap_ctx *c);
uint32_t a_fileMgr_stub(trap_ctx *c);
uint32_t a_cbk_open_file(trap_ctx *c);
uint32_t a_cbk_file_release(trap_ctx *c);
uint32_t a_cbk_file_read(trap_ctx *c);
uint32_t a_cbk_file_write(trap_ctx *c);
uint32_t a_cbk_file_seek(trap_ctx *c);
uint32_t a_cbk_file_size(trap_ctx *c);

/* -------------------- handlers：系统服务（trap_handlers_sys.c）-------------------- */

/* shell */
uint32_t a_zm_shell_AddRef(trap_ctx *c);
uint32_t a_zm_shell_Release(trap_ctx *c);
uint32_t a_zm_shell_GetDeviceInfo(trap_ctx *c);
uint32_t a_zm_shell_GetRootDir(trap_ctx *c);
uint32_t a_zm_shell_GetWorkDir(trap_ctx *c);
uint32_t a_zm_shell_CloseApplet(trap_ctx *c);
uint32_t a_zm_shell_GetApplet(trap_ctx *c);
uint32_t a_zm_shell_GetTickCount(trap_ctx *c);
uint32_t a_zm_shell_UnloadDLL(trap_ctx *c);
uint32_t a_zm_shell_LoadLibraryExt(trap_ctx *c);
uint32_t a_shell_CreateInstance(trap_ctx *c);
uint32_t a_shell_LoadDLL(trap_ctx *c);
uint32_t a_shell_GetAppDir(trap_ctx *c);
uint32_t a_shell_stub(trap_ctx *c);

/* timer */
uint32_t a_zm_timer_StartTimer(trap_ctx *c);
uint32_t a_zm_timer_StopTimer(trap_ctx *c);
uint32_t a_zm_timer_CancelTimer(trap_ctx *c);
uint32_t a_zm_timer_CancelOwnerTimer(trap_ctx *c);
uint32_t a_shell_SetTimer(trap_ctx *c);

/* media */
uint32_t a_media_play(trap_ctx *c);
uint32_t a_media_x54(trap_ctx *c);
uint32_t a_media_AddRef(trap_ctx *c);
uint32_t a_media_Release(trap_ctx *c);
uint32_t a_media_x38(trap_ctx *c);
uint32_t a_media_x40(trap_ctx *c);
uint32_t a_zm_media_stop(trap_ctx *c);
uint32_t a_zm_media_pause_music(trap_ctx *c);
uint32_t a_zm_media_resume_music(trap_ctx *c);
uint32_t a_media_stub(trap_ctx *c);

/* setting */
uint32_t a_setting_AddRef(trap_ctx *c);
uint32_t a_setting_x18(trap_ctx *c);
uint32_t a_setting_x1C(trap_ctx *c);
uint32_t a_setting_x20(trap_ctx *c);
uint32_t a_setting_x24(trap_ctx *c);
uint32_t a_setting_stub(trap_ctx *c);

/* 服务对象（svc / netmgr / tapi / zip / util / dll） */
uint32_t a_zm_svc_release(trap_ctx *c);
uint32_t a_zm_netmgr_x1C(trap_ctx *c);
uint32_t a_zm_tapi_x2C(trap_ctx *c);
uint32_t a_zm_tapi_x40(trap_ctx *c);
uint32_t d_tapi_stub(trap_ctx *c);
uint32_t d_zip_stub(trap_ctx *c);
uint32_t d_util_stub(trap_ctx *c);
uint32_t a_zm_dll_init(trap_ctx *c);
uint32_t a_dll_config(trap_ctx *c);
uint32_t a_dll_entry(trap_ctx *c);

#endif

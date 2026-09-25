/**
 * @file trap_handlers_sys.c
 * @brief 系统服务类槽位：IShell / 定时器 / IMedia / ISetting /
 *        INetMgr / ITAPI / ZIP / IUtil / DLL
 *
 * 这一族的共同点：大多数槽没实现，走 *_stub(off) 记日志返回 0。
 * 未实现槽的偏移由**分派表项**提供（c->off），所以本文件里没有一个
 * AD_OFF(zm_shell_stub, 0x0C, shell_0C) 式的宏展开。
 */

#include "../emu.h"
#include "../log/log.h"
#include "../tool/uc_helper.h" /* uc_read32（读栈上第 4 个参数） */
#include "../zmaee/audio/zm_audio.h" /* IMedia（音频） */
#include "../zmaee/runtime/shell/zm_shell.h"
#include "../zmaee/runtime/timer/zm_timer.h" /* IShell 定时器子系统 */
#include "trap_internal.h"

/* ==========================================================================
 * IShell
 * ========================================================================== */

uint32_t a_zm_shell_AddRef(trap_ctx *c) { return zm_shell_AddRef(c->uc, c->r0); }

uint32_t a_zm_shell_Release(trap_ctx *c) { return zm_shell_Release(c->uc, c->r0); }

uint32_t a_zm_shell_GetDeviceInfo(trap_ctx *c) { return zm_shell_GetDeviceInfo(c->uc, c->r1); }

uint32_t a_zm_shell_GetRootDir(trap_ctx *c) { return zm_shell_GetRootDir(c->uc); }

uint32_t a_zm_shell_GetWorkDir(trap_ctx *c) { return zm_shell_GetWorkDir(c->uc); }

uint32_t a_zm_shell_CloseApplet(trap_ctx *c) { return zm_shell_CloseApplet(c->uc, c->r1); }

uint32_t a_zm_shell_GetApplet(trap_ctx *c) { return zm_shell_GetApplet(c->uc, c->r1); }

uint32_t a_zm_shell_GetTickCount(trap_ctx *c) { return zm_shell_GetTickCount(c->uc); }

uint32_t a_zm_shell_UnloadDLL(trap_ctx *c) { return zm_shell_UnloadDLL(c->uc, c->r0); }

uint32_t a_zm_shell_LoadLibraryExt(trap_ctx *c) {
  return zm_shell_LoadLibraryExt(c->uc, c->r0, c->r1, c->r2, c->r3);
}

/* ---- 参数不齐（r0 是 this）的几个槽 ---- */

uint32_t a_shell_CreateInstance(trap_ctx *c) {
  return zm_shell_CreateInstance(c->uc, c->r1, c->r2);
}

uint32_t a_shell_LoadDLL(trap_ctx *c) {
  return zm_shell_LoadDLL(c->uc, c->r1, c->r2, c->r3);
}

uint32_t a_shell_GetAppDir(trap_ctx *c) { return zm_shell_GetAppDir(c->uc, c->r1, c->r2); }

/* 未实现的 IShell 槽（+0x0C / +0x18 / +0x20 …）：偏移由表项提供 */
uint32_t a_shell_stub(trap_ctx *c) {
  return zm_shell_stub(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}

/* ==========================================================================
 * 定时器
 * ========================================================================== */

/* ROOT+0x148/0x14C：applet 级周期定时器（ZMAEE_Start_Timer/Stop_Timer）
 * 签名 RE：Start_Timer(r0=interval_ms, r1=id, r2=cb)；Stop_Timer(r0=id) */
uint32_t a_zm_timer_StartTimer(trap_ctx *c) {
  return zm_timer_StartTimer(c->uc, c->r0, c->r1, c->r2);
}

uint32_t a_zm_timer_StopTimer(trap_ctx *c) { return zm_timer_StopTimer(c->uc, c->r0); }

uint32_t a_zm_timer_CancelTimer(trap_ctx *c) { return zm_timer_CancelTimer(c->uc, c->r1); }

uint32_t a_zm_timer_CancelOwnerTimer(trap_ctx *c) {
  return zm_timer_CancelOwnerTimer(c->uc, c->r1);
}

/* IShell.SetTimer：第 4 个参数在栈上 */
uint32_t a_shell_SetTimer(trap_ctx *c) {
  return zm_timer_SetTimer(c->uc, c->r1, c->r2, c->r3, uc_read32(c->uc, c->sp));
}

/* ==========================================================================
 * IMedia
 * ========================================================================== */

uint32_t a_media_play(trap_ctx *c) {
  return zm_media_command(c->uc, c->r0, c->r1, c->r2, c->r3, c->sp);
}

uint32_t a_media_x54(trap_ctx *c) {
  return zm_media_command(c->uc, c->r0, c->r1, c->r2, c->r3, c->sp);
}

uint32_t a_media_AddRef(trap_ctx *c) {
  (void)c;
  log_info("IMedia.AddRef called");
  return 1;
}

uint32_t a_media_Release(trap_ctx *c) {
  log_info("IMedia.Release called");
  return zm_svc_release(c->uc);
}

uint32_t a_media_x38(trap_ctx *c) {
  (void)c;
  return (uint32_t)-1;
}

uint32_t a_media_x40(trap_ctx *c) {
  (void)c;
  return (uint32_t)-1;
}

uint32_t a_zm_media_stop(trap_ctx *c) { return zm_media_stop(c->uc); }

uint32_t a_zm_media_pause_music(trap_ctx *c) { return zm_media_pause_music(c->uc); }

uint32_t a_zm_media_resume_music(trap_ctx *c) { return zm_media_resume_music(c->uc); }

/* 未实现的 IMedia 槽：偏移由表项提供 */
uint32_t a_media_stub(trap_ctx *c) {
  return zm_media_stub(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}

/* ==========================================================================
 * ISetting
 * ========================================================================== */

uint32_t a_setting_AddRef(trap_ctx *c) {
  (void)c;
  return 1;
}

uint32_t a_setting_x18(trap_ctx *c) {
  log_info("ISetting[0x18] lr=0x%X on=%u", c->lr, c->r1);
  return zm_setting_set_sound(c->uc, c->r1);
}

uint32_t a_setting_x1C(trap_ctx *c) {
  if (g_disasm)
    log_debug("ISetting[0x1C] 调用点 lr=0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X",
              c->lr, c->r0, c->r1, c->r2, c->r3);
  return zm_setting_stub(c->uc, 0x1C, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_setting_x20(trap_ctx *c) {
  if (g_disasm)
    log_debug("ISetting[0x20] 调用点 lr=0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X",
              c->lr, c->r0, c->r1, c->r2, c->r3);
  return zm_setting_stub(c->uc, 0x20, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_setting_x24(trap_ctx *c) { return zm_setting_x24(c->uc, c->r1, c->r2); }

/* 未实现的 ISetting 槽：偏移由表项提供 */
uint32_t a_setting_stub(trap_ctx *c) {
  return zm_setting_stub(c->uc, c->off, c->r0, c->r1, c->r2, c->r3);
}

/* ==========================================================================
 * 服务对象：svc / netmgr / tapi / zip / util / dll
 * ========================================================================== */

uint32_t a_zm_svc_release(trap_ctx *c) { return zm_svc_release(c->uc); }

uint32_t a_zm_netmgr_x1C(trap_ctx *c) {
  return zm_netmgr_x1C(c->uc, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_zm_tapi_x2C(trap_ctx *c) {
  return zm_tapi_x2C(c->uc, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_zm_tapi_x40(trap_ctx *c) {
  return zm_tapi_x40(c->uc, c->r0, c->r1, c->r2, c->r3);
}

/* 区间 stub：offset 由 trap 地址自动算 */
uint32_t d_tapi_stub(trap_ctx *c) {
  return zm_tapi_stub(c->uc, c->trap - TAPI_VT_ADDR, c->r0, c->r1, c->r2, c->r3);
}

uint32_t d_zip_stub(trap_ctx *c) {
  return zm_zip_stub(c->uc, c->trap - ZIP_VT_ADDR, c->r0, c->r1, c->r2, c->r3);
}

uint32_t d_util_stub(trap_ctx *c) {
  return zm_util_stub(c->uc, c->trap - TRAMP_BASE, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_zm_dll_init(trap_ctx *c) { return zm_dll_init(c->uc); }

uint32_t a_dll_config(trap_ctx *c) { return zm_dll_config(c->uc, c->r1, c->r2, c->r3); }

uint32_t a_dll_entry(trap_ctx *c) { return zm_dll_entry(c->uc, c->r1, c->r2, c->r3); }

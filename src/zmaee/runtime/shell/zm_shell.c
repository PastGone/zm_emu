#include "zm_shell.h"

#include "../../../emu.h"
#include "../../../log/log.h"
#include "../../../tool/uc_helper.h"
#include "../../core/zm_root.h" /* zm_root_get_tick（SDL_GetTicks） */
#include "../../core/zm_str.h"  /* read_cstr */
#include <stdint.h>

/* =========================================================================
 * ZMAEE IShell 原生虚表处理函数（g_aee_shell_vtbl @ .data:0x64440，34 槽）
 *
 * shell 为全局单例：root.getShell() 返回 SHELL 对象（SHIM+0x100），
 * 其 vptr 指向 SHELL_VT（SHIM+0x180）。旧 RT_VT / zm_rt_* 是早期误命名，
 * 已更名对齐（CreateInstance/GetDeviceInfo/LoadLibraryExt 均为 IShell 方法）。
 *
 * 原则：任何槽被调用都不应落到 "非法的外部调用" 而卡死 pause_console。
 *   - 实测过行为的槽（CreateInstance/GetDeviceInfo/LoadDLL/UnloadDLL/
 *     LoadLibraryExt/GetTickCount）按真实行为实现；
 *   - 其余接 zm_shell_stub：仅记录日志、返回 0。
 * ========================================================================= */

/* 通用 stub：记录 offset 与参数，返回 0（不崩） */
uint32_t zm_shell_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                       uint32_t r2, uint32_t r3) {
  (void)uc;
  log_debug("shell stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1,
            r2, r3);
  return 0;
}

/* +0x00 AddRef：单例返回 1 */
uint32_t zm_shell_AddRef(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 1;
}

/* +0x04 Release：无操作 */
uint32_t zm_shell_Release(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 0;
}

/* +0x08 CreateInstance(this, classID, out_ptr)
 * CLSID 表严格按 RE 反编译 ZMAEE_IShell_CreateInstance：
 *   16777219=0x1000003 IFileMgr     16777220=0x1000004 INetMgr
 *   16777221=0x1000005 IDisplay     16777222=0x1000006 IGps
 *   16777223=0x1000007 IGSensor     16777225=0x1000009 ITAPI
 *   16777226=0x100000A IAddrBook    16777227=0x100000B ISetting(变体)
 *   16777228=0x100000C IMedia       16777230=0x100000E IMemStream
 *   16777231=0x100000F IZip         16777232=0x1000010 IStatusBar
 *   16777235=0x1000013 IUtil        default: *out=0, 返回 -3
 * （该变体无 0x1000008/0xD/0x11/0x12。）
 * 尚无对象实现的 CLSID 按 default 语义失败（*out=0，-3），applet 可优雅回退。 */
uint32_t zm_shell_CreateInstance(uc_engine *uc, uint32_t svc, uint32_t out_ptr) {
  uint32_t outobj = 0;
  int32_t ret = 0;
  switch (svc) {
  case 0x1000003: /* IFileMgr */
    outobj = FileMgr;
    break;
  case 0x1000004: /* INetMgr */
    outobj = NETMGR;
    break;
  case 0x1000005: /* IDisplay 全局单例（原生 g_aee_display_vtbl；旧 GFX/GFX_VT
                     是同一张表的早期误命名，已并入 display） */
    outobj = DISPLAY;
    break;
  case 0x1000009: /* ITAPI */
    outobj = TAPI;
    break;
  case 0x100000B:
    /* RE 变体把 0x100000B 分给 ISetting；但目标 applet 实测：该对象被调
     * +0x14 stop / +0x24 get_status，行为是音频控制，故保留 AUDIO 接线。
     * 真实归属待目标固件（非此变体）反汇编确认。 */
    outobj = AUDIO;
    break;
  case 0x100000C: /* IMedia（即原 AP 对象：play/stop，语义吻合） */
    outobj = AP;
    break;
  case 0x1000006: /* IGps      —— 尚未实现 */
  case 0x1000007: /* IGSensor  —— 尚未实现 */
  case 0x100000A: /* IAddrBook —— 尚未实现 */
  case 0x100000E: /* IMemStream—— 尚未实现 */
  case 0x100000F: /* IZip      —— 尚未实现 */
  case 0x1000010: /* IStatusBar—— 尚未实现 */
  case 0x1000013: /* IUtil     —— 尚未实现 */
  default:
    outobj = 0;
    ret = -3; /* RE default 语义 */
    break;
  }
  if (out_ptr)
    uc_write32(uc, out_ptr, outobj);
  log_info("IShell.CreateInstance(svc=0x%X) -> obj=0x%X ret=%d", svc, outobj,
           ret);
  return (uint32_t)ret;
}

/* +0x10 GetDeviceInfo：写 {0, 0, ScreenW, ScreenH} */
uint32_t zm_shell_GetDeviceInfo(uc_engine *uc, uint32_t out_ptr) {
  uc_write32(uc, out_ptr, 0);
  uc_write32(uc, out_ptr + 4, 0);
  uc_write32(uc, out_ptr + 8, g_header.ScreenW);
  uc_write32(uc, out_ptr + 12, g_header.ScreenH);
  return 0;
}

/* +0x48 GetTickCount：单调毫秒时间戳 */
uint32_t zm_shell_GetTickCount(uc_engine *uc) {
  return zm_root_get_tick(uc); /* SDL_GetTicks */
}

/* +0x58 LoadDLL（RE sub_35230）：stub，返回 DLL_OBJ */
uint32_t zm_shell_LoadDLL(uc_engine *uc, uint32_t name_ptr, uint32_t name_len,
                          uint32_t out_ptr) {
  char name[64];
  uint32_t n = name_len < sizeof(name) - 1 ? name_len : sizeof(name) - 1;
  read_cstr(uc, name_ptr, name, n + 1);
  name[n] = '\0';
  log_info("IShell.LoadDLL(\"%s\", len=%u) -> DLL_OBJ (stub)", name, name_len);
  if (out_ptr)
    uc_write32(uc, out_ptr, DLL_OBJ);
  return DLL_OBJ; /* 非 0 表成功 */
}

/* +0x5C UnloadDLL（RE sub_346D8）：stub */
uint32_t zm_shell_UnloadDLL(uc_engine *uc, uint32_t handle) {
  (void)uc;
  log_info("IShell.UnloadDLL(0x%X) stub", handle);
  return 0;
}

/* +0x78 LoadLibraryExt（旧称 loadDLL2）
 * sub_83E24 用它载入 zmsys006.dll：返回非 0 且 *out_obj_ptr 非 0 才算成功，
 * 随后调 (*out_obj_ptr)->vt[0x0C]。
 *
 * 返回 0（失败）使 sub_83E24 返回 nullptr → sub_83F50 返回 false →
 * sub_8433C 返回 false → sub_82AB0 走 sub_82424 绘制 applet 自身 UI。
 * （DLL 真实执行需单独工程；此处让 applet 回退到自带 UI 绘制路径。） */
uint32_t zm_shell_LoadLibraryExt(uc_engine *uc, uint32_t r0, uint32_t buf,
                                 uint32_t size, uint32_t out_obj_ptr) {
  (void)r0;
  (void)buf;
  (void)size;
  (void)uc;
  if (out_obj_ptr)
    uc_write32(uc, out_obj_ptr, 0); /* out=0 → sub_83E24 判定失败 */
  log_info("IShell.LoadLibraryExt(out_ptr=0x%X, size=%u) -> 0 (fail, applet 自绘 UI)",
           out_obj_ptr, size);
  return 0; /* 0 表失败 → applet 走自带绘制路径 */
}

/* ---- 服务对象（CreateInstance 返回的 NETMGR / TAPI） ---- */

/* NETMGR_VT[+4] / TAPI_VT[+4] release */
uint32_t zm_svc_release(uc_engine *uc) {
  (void)uc;
  return 0;
}

/* NETMGR_VT[+0x1C] stub */
uint32_t zm_netmgr_x1C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                       uint32_t r3) {
  (void)uc;
  log_info("stub netmgr[0x1C] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

/* TAPI_VT[+0x2C] stub */
uint32_t zm_tapi_x2C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                     uint32_t r3) {
  (void)uc;
  log_info("stub tapi[0x2C] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

/* TAPI_VT[+0x40] stub */
uint32_t zm_tapi_x40(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                     uint32_t r3) {
  (void)uc;
  log_info("stub tapi[0x40] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

/* ---- stub DLL 对象 vtable 方法（loadDLL 返回的 DLL_OBJ） ---- */

uint32_t zm_dll_init(uc_engine *uc) {
  (void)uc;
  log_info("stub dll init(+8)");
  return 0;
}

uint32_t zm_dll_config(uc_engine *uc, uint32_t a1, uint32_t a2, uint32_t a3) {
  (void)uc;
  log_info("stub dll config(+0xC) a1=%u a2=%u a3=%u", a1, a2, a3);
  return 0;
}

uint32_t zm_dll_entry(uc_engine *uc, uint32_t a1, uint32_t a2, uint32_t a3) {
  (void)uc;
  log_info("stub dll entry(+0x10) a1=%u a2=%u a3=%u", a1, a2, a3);
  return 0;
}

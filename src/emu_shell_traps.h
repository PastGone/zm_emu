#ifndef EMU_SHELL_TRAPS_H
#define EMU_SHELL_TRAPS_H

#include "emu_mem_layout.h"

/* -------------------- SHELL_VT_ADDR 枚举 -------------------- */
/* ZMAEE IShell 原生虚表槽位（基址 SHELL_VT_ADDR，34 槽，止于 +0x88） */
enum ZM_SHELL_VT {
  ZM_Shell_AddRef = 0x00U,
  ZM_Shell_Release = 0x04U,
  ZM_Shell_CreateInstance = 0x08U,
  ZM_Shell_x0C = 0x0CU,
  ZM_Shell_GetDeviceInfo = 0x10U,
  ZM_Shell_GetRootDir = 0x14U,
  ZM_Shell_SetWorkDir = 0x18U,
  ZM_Shell_GetWorkDir = 0x1CU,
  ZM_Shell_StartApplet = 0x20U,
  ZM_Shell_x24 = 0x24U,
  ZM_Shell_CanStartApplet = 0x28U,
  ZM_Shell_ActiveApplet = 0x2CU,
  ZM_Shell_GetApplet = 0x30U,
  ZM_Shell_x34 = 0x34U,
  ZM_Shell_x38 = 0x38U,
  ZM_Shell_SetTimer = 0x3CU,
  ZM_Shell_CancelTimer = 0x40U,
  ZM_Shell_CancelOwnerTimer = 0x44U,
  ZM_Shell_GetTickCount = 0x48U,
  ZM_Shell_OpenWapBrowser = 0x4CU,
  ZM_Shell_x50 = 0x50U,
  ZM_Shell_SetEndKeyMask = 0x54U,
  ZM_Shell_LoadDLL = 0x58U,
  ZM_Shell_UnloadDLL = 0x5CU,
  ZM_Shell_GetAppletMask = 0x60U,
  ZM_Shell_SetAppletMask = 0x64U,
  ZM_Shell_IsLoadGlobalLibrary = 0x68U,
  ZM_Shell_LoadGlobalLibrary = 0x6CU,
  ZM_Shell_FreeGlobalLibrary = 0x70U,
  ZM_Shell_IsGlobalLibraryUseStaticMem = 0x74U,
  ZM_Shell_LoadLibraryExt = 0x78U,
  ZM_Shell_EntryApplet = 0x7CU,
  ZM_Shell_GetAppDir = 0x80U,
  ZM_Shell_GetSupportHall = 0x84U
};

/* ---- ZMAEE IShell 原生虚表（g_aee_shell_vtbl @ .data:0x64440，34 槽）----
 * +0x08 CreateInstance（旧称 queryInterface）、+0x10 GetDeviceInfo（旧称
 * getSystemInfo）、+0x58 LoadDLL（RE sub_35230）、+0x5C UnloadDLL
 * （RE sub_346D8）、+0x78 LoadLibraryExt（旧称 loadDLL2）此前已按行为
 * 实现；其余槽接 zm_shell_stub，保证不落 "非法的外部调用"。 */
#define TR_shell_AddRef TRAP(SHELL_VT_ADDR + ZM_Shell_AddRef)
#define TR_shell_Release TRAP(SHELL_VT_ADDR + ZM_Shell_Release)
#define TR_shell_CreateInstance TRAP(SHELL_VT_ADDR + ZM_Shell_CreateInstance)

#define TR_shell_x0C                                                           \
  TRAP(SHELL_VT_ADDR + ZM_Shell_x0C) /* RE sub_34DE4，未知                  \
                                      */

#define TR_shell_GetDeviceInfo TRAP(SHELL_VT_ADDR + ZM_Shell_GetDeviceInfo)
#define TR_shell_GetRootDir TRAP(SHELL_VT_ADDR + ZM_Shell_GetRootDir)
#define TR_shell_SetWorkDir TRAP(SHELL_VT_ADDR + ZM_Shell_SetWorkDir)
#define TR_shell_GetWorkDir TRAP(SHELL_VT_ADDR + ZM_Shell_GetWorkDir)
#define TR_shell_StartApplet TRAP(SHELL_VT_ADDR + ZM_Shell_StartApplet)

#define TR_shell_x24                                                           \
  TRAP(SHELL_VT_ADDR + ZM_Shell_x24) /* RE sub_3482C，未知                  \
                                      */

#define TR_shell_CanStartApplet TRAP(SHELL_VT_ADDR + ZM_Shell_CanStartApplet)
#define TR_shell_ActiveApplet TRAP(SHELL_VT_ADDR + ZM_Shell_ActiveApplet)
#define TR_shell_GetApplet TRAP(SHELL_VT_ADDR + ZM_Shell_GetApplet)

#define TR_shell_x34                                                           \
  TRAP(SHELL_VT_ADDR + ZM_Shell_x34) /* RE sub_34764，未知                  \
                                      */

#define TR_shell_x38                                                           \
  TRAP(SHELL_VT_ADDR + ZM_Shell_x38) /* RE sub_34C1C，未知                  \
                                      */

#define TR_shell_SetTimer TRAP(SHELL_VT_ADDR + ZM_Shell_SetTimer)
#define TR_shell_CancelTimer TRAP(SHELL_VT_ADDR + ZM_Shell_CancelTimer)
#define TR_shell_CancelOwnerTimer                                              \
  TRAP(SHELL_VT_ADDR + ZM_Shell_CancelOwnerTimer)
#define TR_shell_GetTickCount TRAP(SHELL_VT_ADDR + ZM_Shell_GetTickCount)
#define TR_shell_OpenWapBrowser TRAP(SHELL_VT_ADDR + ZM_Shell_OpenWapBrowser)

#define TR_shell_x50                                                           \
  TRAP(SHELL_VT_ADDR + ZM_Shell_x50) /* RE sub_3474C，未知                  \
                                      */

#define TR_shell_SetEndKeyMask TRAP(SHELL_VT_ADDR + ZM_Shell_SetEndKeyMask)
#define TR_shell_LoadDLL TRAP(SHELL_VT_ADDR + ZM_Shell_LoadDLL)
#define TR_shell_UnloadDLL TRAP(SHELL_VT_ADDR + ZM_Shell_UnloadDLL)
#define TR_shell_GetAppletMask TRAP(SHELL_VT_ADDR + ZM_Shell_GetAppletMask)
#define TR_shell_SetAppletMask TRAP(SHELL_VT_ADDR + ZM_Shell_SetAppletMask)
#define TR_shell_IsLoadGlobalLibrary                                           \
  TRAP(SHELL_VT_ADDR + ZM_Shell_IsLoadGlobalLibrary)
#define TR_shell_LoadGlobalLibrary                                             \
  TRAP(SHELL_VT_ADDR + ZM_Shell_LoadGlobalLibrary)
#define TR_shell_FreeGlobalLibrary                                             \
  TRAP(SHELL_VT_ADDR + ZM_Shell_FreeGlobalLibrary)
#define TR_shell_IsGlobalLibraryUseStaticMem                                   \
  TRAP(SHELL_VT_ADDR + ZM_Shell_IsGlobalLibraryUseStaticMem)
#define TR_shell_LoadLibraryExt TRAP(SHELL_VT_ADDR + ZM_Shell_LoadLibraryExt)
#define TR_shell_EntryApplet TRAP(SHELL_VT_ADDR + ZM_Shell_EntryApplet)
#define TR_shell_GetAppDir TRAP(SHELL_VT_ADDR + ZM_Shell_GetAppDir)
#define TR_shell_GetSupportHall TRAP(SHELL_VT_ADDR + ZM_Shell_GetSupportHall)

#endif /* EMU_SHELL_TRAPS_H */
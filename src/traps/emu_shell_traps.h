#ifndef EMU_SHELL_TRAPS_H
#define EMU_SHELL_TRAPS_H

#include "../emu_mem_layout.h"

/* -------------------- SHELL_VT_ADDR 枚举 -------------------- */
/* ZMAEE IShell 原生虚表槽位（基址 SHELL_VT_ADDR，34 槽，止于 +0x88）
 *
 * 槽位本身来自固件虚表 .data:0x64440（34 个函数指针，已 dump 核对）。
 *
 * 名字的证据分两档，注释里标了 ✅ / ❓：
 *   ✅ 已证  —— 从该槽指向的函数里解出的**字符串字面量**得名
 *             （如 +0x24 sub_3482C 里的日志串 "CloseApplet: bRetToIdle = %d"；
 *              IMedia 那批则是真的 JNI 方法名字面量 + 签名）。
 *   ❓ 疑似  —— 固件里存在同名字符串（下面【疑名清单】），但**没有**建立
 *             "字符串 ↔ 该槽位"的引用链（Thumb/ARM 全扫无 PC 相对引用，
 *             也不在虚表旁边的名字表里），属历史沿用命名，待证。
 */
/*
 * 【疑名清单】固件中出现的 ZMAEE_IShell_* 字符串（均未建立引用链，仅供对照）
 *   0x05DBA  ZMAEE_IShell_ActiveApplet
 *   0x05DE5  ZMAEE_IShell_GetApplet
 *   0x0645B  ZMAEE_IShell_StartApplet
 *   0x081DA  ZMAEE_IShell_EntryApplet
 *   0x081F3  ZMAEE_GetFixedApplet
 *   0x0823E  ZMAEE_IShell_StartAppletROM
 *   0x0825A  ZMAEE_IShell_StartAppletROM_Internal
 *   0x082B2  ZMAEE_GetApplet
 *   0x082C2  ZMAEE_IShell_GetAppletMask
 *   0x082DD  ZMAEE_IShell_SetAppletMask
 *   0x083A4  ZMAEE_IShell_CanStartApplet
 *   0x083C0  ZMAEE_IShell_ValidateApplet
 *   0x08458  ZMAEE_IShell_RunApplet
 *   0x0846F  ZMAEE_IShell_StartApplet_Internal
 *   0x5C43C  "CloseApplet: bRetToIdle = %d"   ← ✅ 已证，属 +0x24
 */
enum ZM_SHELL_VT : uint32_t {
  ZM_Shell_AddRef = 0x00U,
  ZM_Shell_Release = 0x04U,
  ZM_Shell_CreateInstance = 0x08U,
  ZM_Shell_x0C = 0x0CU,
  ZM_Shell_GetDeviceInfo = 0x10U,
  ZM_Shell_GetRootDir = 0x14U,
  ZM_Shell_SetWorkDir = 0x18U,
  ZM_Shell_GetWorkDir = 0x1CU,
  /* ❓ 疑名：固件串 ZMAEE_IShell_StartApplet @0x0645B（无引用链，待证）
   *          对应函数 sub_3592C（未解出可自证的字符串） */
  ZM_Shell_StartApplet = 0x20U,

  /* ✅ 已证 +0x24 = CloseApplet(bRetToIdle)：
   *   该槽指向 sub_3482C，其日志串就是 "CloseApplet: bRetToIdle = %d"（@0x5C43C）。
   *   applet 用它请求关闭自己（00000506 点标题页"退出"那块会调它）。 */
  ZM_Shell_CloseApplet = 0x24U,

  /* ❓ 疑名：ZMAEE_IShell_CanStartApplet @0x083A4（无引用链）→ sub_34D60 */
  ZM_Shell_CanStartApplet = 0x28U,

  /* ❓ 疑名：ZMAEE_IShell_ActiveApplet @0x05DBA（无引用链）→ sub_345F8 */
  ZM_Shell_ActiveApplet = 0x2CU,

  /* ❓ 疑名：ZMAEE_IShell_GetApplet @0x05DE5（无引用链）→ sub_3461C */
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
enum ZM_SHELL_TRAPS : uint32_t {
  TR_shell_AddRef = TRAP(SHELL_VT_ADDR + ZM_Shell_AddRef),
  TR_shell_Release = TRAP(SHELL_VT_ADDR + ZM_Shell_Release),
  TR_shell_CreateInstance = TRAP(SHELL_VT_ADDR + ZM_Shell_CreateInstance),
  TR_shell_x0C = TRAP(SHELL_VT_ADDR + ZM_Shell_x0C), /* RE sub_34DE4，未知 */
  TR_shell_GetDeviceInfo = TRAP(SHELL_VT_ADDR + ZM_Shell_GetDeviceInfo),
  TR_shell_GetRootDir = TRAP(SHELL_VT_ADDR + ZM_Shell_GetRootDir),
  TR_shell_SetWorkDir = TRAP(SHELL_VT_ADDR + ZM_Shell_SetWorkDir),
  TR_shell_GetWorkDir = TRAP(SHELL_VT_ADDR + ZM_Shell_GetWorkDir),
  TR_shell_StartApplet = TRAP(SHELL_VT_ADDR + ZM_Shell_StartApplet),
  /* +0x24 CloseApplet(bRetToIdle)：RE sub_3482C（日志串 "CloseApplet: bRetToIdle
   * = %d"）。applet 请求关闭自己 → zm_shell_CloseApplet 处理。 */
  TR_shell_CloseApplet = TRAP(SHELL_VT_ADDR + ZM_Shell_CloseApplet),
  TR_shell_CanStartApplet = TRAP(SHELL_VT_ADDR + ZM_Shell_CanStartApplet),
  TR_shell_ActiveApplet = TRAP(SHELL_VT_ADDR + ZM_Shell_ActiveApplet),
  TR_shell_GetApplet = TRAP(SHELL_VT_ADDR + ZM_Shell_GetApplet),
  TR_shell_x34 = TRAP(SHELL_VT_ADDR + ZM_Shell_x34), /* RE sub_34764，未知 */
  TR_shell_x38 = TRAP(SHELL_VT_ADDR + ZM_Shell_x38), /* RE sub_34C1C，未知 */
  TR_shell_SetTimer = TRAP(SHELL_VT_ADDR + ZM_Shell_SetTimer),
  TR_shell_CancelTimer = TRAP(SHELL_VT_ADDR + ZM_Shell_CancelTimer),
  TR_shell_CancelOwnerTimer = TRAP(SHELL_VT_ADDR + ZM_Shell_CancelOwnerTimer),
  TR_shell_GetTickCount = TRAP(SHELL_VT_ADDR + ZM_Shell_GetTickCount),
  TR_shell_OpenWapBrowser = TRAP(SHELL_VT_ADDR + ZM_Shell_OpenWapBrowser),
  TR_shell_x50 = TRAP(SHELL_VT_ADDR + ZM_Shell_x50), /* RE sub_3474C，未知 */
  TR_shell_SetEndKeyMask = TRAP(SHELL_VT_ADDR + ZM_Shell_SetEndKeyMask),
  TR_shell_LoadDLL = TRAP(SHELL_VT_ADDR + ZM_Shell_LoadDLL),
  TR_shell_UnloadDLL = TRAP(SHELL_VT_ADDR + ZM_Shell_UnloadDLL),
  TR_shell_GetAppletMask = TRAP(SHELL_VT_ADDR + ZM_Shell_GetAppletMask),
  TR_shell_SetAppletMask = TRAP(SHELL_VT_ADDR + ZM_Shell_SetAppletMask),
  TR_shell_IsLoadGlobalLibrary = TRAP(SHELL_VT_ADDR + ZM_Shell_IsLoadGlobalLibrary),
  TR_shell_LoadGlobalLibrary = TRAP(SHELL_VT_ADDR + ZM_Shell_LoadGlobalLibrary),
  TR_shell_FreeGlobalLibrary = TRAP(SHELL_VT_ADDR + ZM_Shell_FreeGlobalLibrary),
  TR_shell_IsGlobalLibraryUseStaticMem = TRAP(SHELL_VT_ADDR + ZM_Shell_IsGlobalLibraryUseStaticMem),
  TR_shell_LoadLibraryExt = TRAP(SHELL_VT_ADDR + ZM_Shell_LoadLibraryExt),
  TR_shell_EntryApplet = TRAP(SHELL_VT_ADDR + ZM_Shell_EntryApplet),
  TR_shell_GetAppDir = TRAP(SHELL_VT_ADDR + ZM_Shell_GetAppDir),
  TR_shell_GetSupportHall = TRAP(SHELL_VT_ADDR + ZM_Shell_GetSupportHall)
};

#endif /* EMU_SHELL_TRAPS_H */

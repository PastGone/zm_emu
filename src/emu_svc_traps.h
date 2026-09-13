#ifndef EMU_SVC_TRAPS_H
#define EMU_SVC_TRAPS_H

#include "emu_mem_layout.h"

/* -------------------- 服务对象虚表枚举 -------------------- */

/* ZMAEE INetMgr 原生虚表槽位（基址 NETMGR_VT_ADDR） */
enum ZM_NETMGR_VT { ZM_NetMgr_release = 0x04U, ZM_NetMgr_x1C = 0x1CU };

/* ZMAEE ITapi 原生虚表槽位（基址 TAPI_VT_ADDR） */
enum ZM_TAPI_VT {
  ZM_Tapi_release = 0x04U,
  ZM_Tapi_x2C = 0x2CU,
  ZM_Tapi_x40 = 0x40U
};

/* DLL 对象虚表槽位（基址 DLL_OBJ_VT_ADDR） */
enum ZM_DLL_OBJ_VT {
  ZM_Dll_init = 0x08U,
  ZM_Dll_config = 0x0CU,
  ZM_Dll_entry = 0x10U
};

/* CBK 对象虚表槽位（基址 CBK_OBJ_VT_ADDR） */
enum ZM_CBK_OBJ_VT { ZM_Cbk_default = 0x08U };

/* ZMAEE IUtil 服务对象虚表槽位（基址 IUTIL_VT_ADDR，7 槽 ×4B = 0x1C） */
enum ZM_IUTIL_VT {
  ZM_Util_x00 = 0x00U,
  ZM_Util_x04 = 0x04U,
  ZM_Util_x08 = 0x08U,
  ZM_Util_x0C = 0x0CU,
  ZM_Util_x10 = 0x10U,
  ZM_Util_x14 = 0x14U,
  ZM_Util_x18 = 0x18U
};

/* ---- 服务对象 / FS / DLL / CBK trap ----
 * 旧「索引 46..57」方案把这些宏挂在 ROOT_TABLE_ADDR+0x2E..0x3F 的伪造索引上，与
 * SHIM↔TRAMP 对射派发不符：applet 经对象虚表发起的真实调用落在各自
 * VT 槽位，伪造索引永不命中（与已修的 loadDLL 同病）。现按真实槽位挂接。
 * fs_chdir 的真实槽位未知，暂缺（其 case 已删，待 RE 后补）。 */
#define TR_netmgr_release TRAP(NETMGR_VT_ADDR + ZM_NetMgr_release)
#define TR_netmgr_x1C TRAP(NETMGR_VT_ADDR + ZM_NetMgr_x1C)

#define TR_tapi_release TRAP(TAPI_VT_ADDR + ZM_Tapi_release)
#define TR_tapi_x2C TRAP(TAPI_VT_ADDR + ZM_Tapi_x2C)
#define TR_tapi_x40 TRAP(TAPI_VT_ADDR + ZM_Tapi_x40)

#define TR_dll_init TRAP(DLL_OBJ_VT_ADDR + ZM_Dll_init)
#define TR_dll_config TRAP(DLL_OBJ_VT_ADDR + ZM_Dll_config)
#define TR_dll_entry TRAP(DLL_OBJ_VT_ADDR + ZM_Dll_entry)

#define TR_cbk_default TRAP(CBK_OBJ_VT_ADDR + ZM_Cbk_default)

/* ---- ZMAEE IUtil 服务对象虚表（7 槽 ×4B = 0x1C）---- */
#define TR_util_x00 TRAP(IUTIL_VT_ADDR + ZM_Util_x00)
#define TR_util_x04 TRAP(IUTIL_VT_ADDR + ZM_Util_x04)
#define TR_util_x08 TRAP(IUTIL_VT_ADDR + ZM_Util_x08)
#define TR_util_x0C TRAP(IUTIL_VT_ADDR + ZM_Util_x0C)
#define TR_util_x10 TRAP(IUTIL_VT_ADDR + ZM_Util_x10)
#define TR_util_x14 TRAP(IUTIL_VT_ADDR + ZM_Util_x14)
#define TR_util_x18 TRAP(IUTIL_VT_ADDR + ZM_Util_x18)

#endif /* EMU_SVC_TRAPS_H */
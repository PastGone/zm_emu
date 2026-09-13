#ifndef EMU_SVC_TRAPS_H
#define EMU_SVC_TRAPS_H

#include "emu_mem_layout.h"

/* -------------------- 服务对象虚表枚举 -------------------- */

/* ZMAEE INetMgr 原生虚表槽位（基址 NETMGR_VT_ADDR） */
enum ZM_NETMGR_VT { ZM_NetMgr_release = 0x04U, ZM_NetMgr_x1C = 0x1CU };

/* ZMAEE ITapi 原生虚表槽位（基址 TAPI_VT_ADDR）。
 * IDA 实测 g_aee_tapi_vtbl @ .data:0x64504，共 21 槽（0x00~0x50）。
 * 仅 release / x2C / x40 已接具体 handler，其余为观测探针槽位。 */
enum ZM_TAPI_VT {
  ZM_Tapi_x00 = 0x00U,
  ZM_Tapi_release = 0x04U,
  ZM_Tapi_x08 = 0x08U,
  ZM_Tapi_x0C = 0x0CU,
  ZM_Tapi_x10 = 0x10U,
  ZM_Tapi_x14 = 0x14U,
  ZM_Tapi_x18 = 0x18U,
  ZM_Tapi_x1C = 0x1CU,
  ZM_Tapi_x20 = 0x20U,
  ZM_Tapi_x24 = 0x24U,
  ZM_Tapi_x28 = 0x28U,
  ZM_Tapi_x2C = 0x2CU,
  ZM_Tapi_x30 = 0x30U,
  ZM_Tapi_x34 = 0x34U,
  ZM_Tapi_x38 = 0x38U,
  ZM_Tapi_x3C = 0x3CU,
  ZM_Tapi_x40 = 0x40U,
  ZM_Tapi_x44 = 0x44U,
  ZM_Tapi_x48 = 0x48U,
  ZM_Tapi_x4C = 0x4CU,
  ZM_Tapi_x50 = 0x50U
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

/* ZMAEE IZip 服务对象虚表槽位（基址 ZIP_VT_ADDR）。
 * IDA 实测 g_aee_zip_vtbl @ .data:0x64574，共 5 槽（0x00~0x10）。 */
enum ZM_ZIP_VT {
  ZM_Zip_x00 = 0x00U,
  ZM_Zip_x04 = 0x04U,
  ZM_Zip_x08 = 0x08U,
  ZM_Zip_x0C = 0x0CU,
  ZM_Zip_x10 = 0x10U
};

/* ---- 服务对象 / FS / DLL / CBK trap ----
 * 旧「索引 46..57」方案把这些宏挂在 ROOT_TABLE_ADDR+0x2E..0x3F 的伪造索引上，与
 * SHIM↔TRAMP 对射派发不符：applet 经对象虚表发起的真实调用落在各自
 * VT 槽位，伪造索引永不命中（与已修的 loadDLL 同病）。现按真实槽位挂接。
 * fs_chdir 的真实槽位未知，暂缺（其 case 已删，待 RE 后补）。 */
#define TR_netmgr_release TRAP(NETMGR_VT_ADDR + ZM_NetMgr_release)
#define TR_netmgr_x1C TRAP(NETMGR_VT_ADDR + ZM_NetMgr_x1C)

#define TR_tapi_x00 TRAP(TAPI_VT_ADDR + ZM_Tapi_x00)
#define TR_tapi_release TRAP(TAPI_VT_ADDR + ZM_Tapi_release)
#define TR_tapi_x08 TRAP(TAPI_VT_ADDR + ZM_Tapi_x08)
#define TR_tapi_x0C TRAP(TAPI_VT_ADDR + ZM_Tapi_x0C)
#define TR_tapi_x10 TRAP(TAPI_VT_ADDR + ZM_Tapi_x10)
#define TR_tapi_x14 TRAP(TAPI_VT_ADDR + ZM_Tapi_x14)
#define TR_tapi_x18 TRAP(TAPI_VT_ADDR + ZM_Tapi_x18)
#define TR_tapi_x1C TRAP(TAPI_VT_ADDR + ZM_Tapi_x1C)
#define TR_tapi_x20 TRAP(TAPI_VT_ADDR + ZM_Tapi_x20)
#define TR_tapi_x24 TRAP(TAPI_VT_ADDR + ZM_Tapi_x24)
#define TR_tapi_x28 TRAP(TAPI_VT_ADDR + ZM_Tapi_x28)
#define TR_tapi_x2C TRAP(TAPI_VT_ADDR + ZM_Tapi_x2C)
#define TR_tapi_x30 TRAP(TAPI_VT_ADDR + ZM_Tapi_x30)
#define TR_tapi_x34 TRAP(TAPI_VT_ADDR + ZM_Tapi_x34)
#define TR_tapi_x38 TRAP(TAPI_VT_ADDR + ZM_Tapi_x38)
#define TR_tapi_x3C TRAP(TAPI_VT_ADDR + ZM_Tapi_x3C)
#define TR_tapi_x40 TRAP(TAPI_VT_ADDR + ZM_Tapi_x40)
#define TR_tapi_x44 TRAP(TAPI_VT_ADDR + ZM_Tapi_x44)
#define TR_tapi_x48 TRAP(TAPI_VT_ADDR + ZM_Tapi_x48)
#define TR_tapi_x4C TRAP(TAPI_VT_ADDR + ZM_Tapi_x4C)
#define TR_tapi_x50 TRAP(TAPI_VT_ADDR + ZM_Tapi_x50)

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

/* ---- ZMAEE IZip 服务对象虚表（5 槽 ×4B = 0x14）---- */
#define TR_zip_x00 TRAP(ZIP_VT_ADDR + ZM_Zip_x00)
#define TR_zip_x04 TRAP(ZIP_VT_ADDR + ZM_Zip_x04)
#define TR_zip_x08 TRAP(ZIP_VT_ADDR + ZM_Zip_x08)
#define TR_zip_x0C TRAP(ZIP_VT_ADDR + ZM_Zip_x0C)
#define TR_zip_x10 TRAP(ZIP_VT_ADDR + ZM_Zip_x10)

#endif /* EMU_SVC_TRAPS_H */
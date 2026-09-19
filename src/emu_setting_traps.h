#ifndef EMU_SETTING_TRAPS_H
#define EMU_SETTING_TRAPS_H

#include "emu_mem_layout.h"

/* -------------------- SETTING_VT_ADDR 枚举 -------------------- */
/* ZMAEE ISetting 原生虚表槽位（基址 SETTING_VT_ADDR，14 槽，止于 +0x38） */
enum ZM_SETTING_VT : uint32_t {
  ZM_Setting_AddRef = 0x00U,
  ZM_Setting_Release = 0x04U,
  ZM_Setting_x08 = 0x08U,
  ZM_Setting_x0C = 0x0CU,
  ZM_Setting_x10 = 0x10U,
  ZM_Setting_x14 = 0x14U,
  ZM_Setting_x18 = 0x18U,
  ZM_Setting_x1C = 0x1CU,
  ZM_Setting_x20 = 0x20U,
  ZM_Setting_x24 = 0x24U,
  ZM_Setting_x28 = 0x28U,
  ZM_Setting_x2C = 0x2CU,
  ZM_Setting_x30 = 0x30U,
  ZM_Setting_x34 = 0x34U
};

/* ---- ZMAEE ISetting 原生虚表（RE：g_aee_setting_vtbl @ .data:0x64408，
 * 14 槽）----
 *   +0x00 sub_33D60  +0x04 sub_34118  +0x08 sub_34050  +0x0C sub_33D74
 *   +0x10 sub_33D78  +0x14 sub_33D7C  +0x18 sub_33FB4  +0x1C sub_33E2C
 *   +0x20 sub_33EFC  +0x24 sub_33F4C  +0x28 sub_33D80  +0x2C sub_33D84
 *   +0x30 sub_33E98  +0x34 sub_33DFC
 * 语义待各自 RE；+0x24 保留"写 0 到 out4/out_buf"的既有行为（applet
 * 依赖它做后续分支判断），其余接 zm_setting_stub。 */
enum ZM_SETTING_TRAPS : uint32_t {
  TR_setting_AddRef = TRAP(SETTING_VT_ADDR + ZM_Setting_AddRef),
  TR_setting_Release = TRAP(SETTING_VT_ADDR + ZM_Setting_Release),
  TR_setting_x08 = TRAP(SETTING_VT_ADDR + ZM_Setting_x08),
  TR_setting_x0C = TRAP(SETTING_VT_ADDR + ZM_Setting_x0C),
  TR_setting_x10 = TRAP(SETTING_VT_ADDR + ZM_Setting_x10),
  TR_setting_x14 = TRAP(SETTING_VT_ADDR + ZM_Setting_x14),
  TR_setting_x18 = TRAP(SETTING_VT_ADDR + ZM_Setting_x18),
  TR_setting_x1C = TRAP(SETTING_VT_ADDR + ZM_Setting_x1C),
  TR_setting_x20 = TRAP(SETTING_VT_ADDR + ZM_Setting_x20),
  TR_setting_x24 = TRAP(SETTING_VT_ADDR + ZM_Setting_x24),
  TR_setting_x28 = TRAP(SETTING_VT_ADDR + ZM_Setting_x28),
  TR_setting_x2C = TRAP(SETTING_VT_ADDR + ZM_Setting_x2C),
  TR_setting_x30 = TRAP(SETTING_VT_ADDR + ZM_Setting_x30),
  TR_setting_x34 = TRAP(SETTING_VT_ADDR + ZM_Setting_x34)
};

#endif /* EMU_SETTING_TRAPS_H */

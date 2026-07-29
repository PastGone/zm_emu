#include "zm_runtime.h"

#include "../../log/log.h"
#include "../core/zm_addrs.h"
#include "../core/zm_common.h"
#include "../core/zm_str.h" /* read_cstr */

/* 屏幕宽高，由 zm_rt_set_screen_size 设置（取自 AppHeader），
 * 使 applet 通过 getSystemInfo 拿到的尺寸与渲染窗口一致。 */
static uint32_t g_screen_w = 240;
static uint32_t g_screen_h = 240;

void zm_rt_set_screen_size(uint32_t w, uint32_t h) {
  if (w)
    g_screen_w = w;
  if (h)
    g_screen_h = h;
}

/**
 * @brief rt.queryInterface：按服务号返回对应子系统对象地址
 *
 * 服务号 -> 对象：
 *   0x1000005 -> GFX
 *   0x1000003 -> FS
 *   0x100000B -> AUDIO
 *   0x100000C -> AP
 *   0x1000004 -> SVC04（00000405.app 新增）
 *   0x1000009 -> SVC09（00000405.app 新增）
 * 其它 -> 0
 */
uint32_t zm_rt_queryInterface(uc_engine *uc, uint32_t svc, uint32_t out_ptr) {
  uint32_t outobj = 0;
  switch (svc) {
  case 0x1000005:
    outobj = GFX;
    break; // GFX
  case 0x1000003:
    outobj = FS;
    break; // FS
  case 0x100000B:
    outobj = AUDIO;
    break; // AUDIO
  case 0x100000C:
    outobj = AP;
    break; // AP
  case 0x1000004:
    outobj = SVC04;
    break; // 00000405
  case 0x1000009:
    outobj = SVC09;
    break; // 00000405
  default:
    outobj = 0;
    break;
  }
  if (out_ptr)
    zm_write32(uc, out_ptr, outobj);
  log_info("queryInterface(0x%X) -> 0x%X", svc, outobj);
  return 0;
}

/**
 * @brief rt.getSystemInfo：写入系统信息到 out_ptr
 *
 * +0=0, +4=0, +8=ScreenW, +12=ScreenH
 * ScreenW/ScreenH 取自 zm_rt_set_screen_size 设置的值（AppHeader），
 * 保证 applet 布局与渲染窗口一致。
 */
uint32_t zm_rt_getSystemInfo(uc_engine *uc, uint32_t out_ptr) {
  zm_write32(uc, out_ptr, 0);
  zm_write32(uc, out_ptr + 4, 0);
  zm_write32(uc, out_ptr + 8, g_screen_w);
  zm_write32(uc, out_ptr + 12, g_screen_h);
  return 0;
}

/* ---- 00000405.app 新增 runtime 接口实现 ---- */

/* RT_VT[+0x58] loadDLL(name_ptr, name_len, out_ptr) */
uint32_t zm_rt_loadDLL(uc_engine *uc, uint32_t name_ptr, uint32_t name_len,
                       uint32_t out_ptr) {
  char name[64];
  uint32_t n = name_len < sizeof(name) - 1 ? name_len : sizeof(name) - 1;
  read_cstr(uc, name_ptr, name, n + 1);
  name[n] = '\0';
  log_info("loadDLL(\"%s\", len=%u) -> DLL_OBJ (stub)", name, name_len);
  if (out_ptr)
    zm_write32(uc, out_ptr, DLL_OBJ);
  return DLL_OBJ; /* 非 0 表成功 */
}

/* RT_VT[+0x5C] unloadDLL(handle) */
uint32_t zm_rt_unloadDLL(uc_engine *uc, uint32_t handle) {
  (void)uc;
  log_info("unloadDLL(0x%X) stub", handle);
  return 0;
}

/* RT_VT[+0x78] loadDLL2(this, buf, size, out_obj_ptr, alloc_buf, alloc_sz)
 * sub_83E24 用它载入 zmsys006.dll：返回非 0 且 *out_obj_ptr 非 0 才算成功，
 * 随后调 (*out_obj_ptr)->vt[0x0C]。
 *
 * 返回 0（失败）使 sub_83E24 返回 nullptr → sub_83F50 返回 false →
 * sub_8433C 返回 false → sub_82AB0 走 sub_82424 绘制 applet 自身 UI。
 * （DLL 真实执行需单独工程；此处让 applet 回退到自带 UI 绘制路径。） */
uint32_t zm_rt_loadDLL2(uc_engine *uc, uint32_t r0, uint32_t buf, uint32_t size,
                        uint32_t out_obj_ptr) {
  (void)r0;
  (void)buf;
  (void)size;
  (void)uc;
  if (out_obj_ptr)
    zm_write32(uc, out_obj_ptr, 0); /* DLL_OBJ=0 → sub_83E24 判定失败 */
  log_info("loadDLL2(out_ptr=0x%X, size=%u) -> 0 (fail, applet 自绘 UI)",
           out_obj_ptr, size);
  return 0; /* 0 表失败 → applet 走自带绘制路径 */
}

/* SVC04_VT[+4] / SVC09_VT[+4] release */
uint32_t zm_svc_release(uc_engine *uc) {
  (void)uc;
  return 0;
}

/* SVC04_VT[+0x1C] stub */
uint32_t zm_svc04_x1C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                      uint32_t r3) {
  (void)uc;
  log_info("stub svc04[0x1C] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

/* SVC09_VT[+0x2C] stub */
uint32_t zm_svc09_x2C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                      uint32_t r3) {
  (void)uc;
  log_info("stub svc09[0x2C] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

/* SVC09_VT[+0x40] stub */
uint32_t zm_svc09_x40(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                      uint32_t r3) {
  (void)uc;
  log_info("stub svc09[0x40] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

/* stub DLL 对象 vtable 方法 */
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

#include "zm_runtime.h"

#include "../core/zm_addrs.h"
#include "../core/zm_common.h"

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
  default:
    outobj = 0;
    break;
  }
  if (out_ptr)
    zm_write32(uc, out_ptr, outobj);
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

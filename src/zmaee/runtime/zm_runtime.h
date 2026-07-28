#ifndef __ZM_RUNTIME_H__
#define __ZM_RUNTIME_H__

// -------------------- runtime 相关 trap 处理函数声明 --------------------
#include <stdint.h>
#include <unicorn/unicorn.h>

/**
 * @brief rt.queryInterface：按服务号返回对应子系统对象地址
 * @param svc     服务号（对应 r1），如 0x1000005=GFX / 0x1000003=FS /
 *                0x100000B=AUDIO / 0x100000C=AP
 * @param out_ptr 输出槽地址（对应 r2），非 0 时把对象地址写入此处
 * @return 固定返回 0
 */
uint32_t zm_rt_queryInterface(uc_engine *uc, uint32_t svc, uint32_t out_ptr);

/**
 * @brief rt.getSystemInfo：写入系统信息（屏幕宽高等）到 out_ptr
 *
 * 写入布局：+0=?, +4=?, +8=ScreenW, +12=ScreenH
 * ScreenW/ScreenH 由 zm_rt_set_screen_size 设置（取自 AppHeader），
 * 默认 240x240。
 *
 * @param out_ptr 输出地址（对应 r1）
 * @return 固定返回 0
 */
uint32_t zm_rt_getSystemInfo(uc_engine *uc, uint32_t out_ptr);

/**
 * @brief 设置屏幕宽高，供 getSystemInfo 返回。
 *        应在解析 AppHeader 后、启动模拟前调用，使 applet 布局与
 *        渲染窗口（AppHeader.ScreenW/ScreenH）一致。
 */
void zm_rt_set_screen_size(uint32_t w, uint32_t h);

#endif

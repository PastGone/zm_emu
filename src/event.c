#include "./event.h"
#include "./log/log.h"
#include "./zmaee/core/zm_root.h"
#include "./zmaee/gfx/zm_gfx.h"
#include "./zmaee/inc/zm_event_code.h"
#include "unicorn/arm.h"
#include <stdbool.h>

/* 待处理的触摸事件队列（仅支持 1 个，case 10 penUp 跟在 case 9 之后） */
static struct {
  bool has_pending;
  uint32_t evt;
  uint32_t x, y;
} g_pending_touch = {false, 0, 0, 0};

uint32_t g_auto_clicks = 0;

/* 已注入的合成点击序号，用来在屏幕上摊开成 3x3 网格 */
static uint32_t s_auto_click_idx = 0;

void dispatch_applet_event(uint32_t evt, uint32_t x, uint32_t y) {
  if (!g_instance || !g_handler) {
    log_warn("事件 %u 无法派发：instance=0x%X handler=0x%X", evt, g_instance,
             g_handler);
    return;
  }

  /* handler 必须每轮重新从 applet 虚表里读——00000405 会在初始化过程中
   * 把 instance->vt[8] 换成自己的钩子（sub_82FF8）并链到原实现，
   * 缓存住旧地址会让后续事件全部走错分支。 */
  {
    uint32_t cur = 0;
    if (uc_mem_read(g_uc, API_SLOT + 8, &cur, 4) == UC_ERR_OK && cur != 0 &&
        cur != g_handler) {
      log_info("applet 更换了事件 handler：0x%08X → 0x%08X", g_handler, cur);
      g_handler = cur;
    }
  }

  uint32_t lr = TR_enter_event_loop;
  uc_reg_write(g_uc, UC_ARM_REG_LR, &lr);

  uc_reg_write(g_uc, UC_ARM_REG_R0, &g_instance);
  uc_reg_write(g_uc, UC_ARM_REG_R1, &evt);
  uc_reg_write(g_uc, UC_ARM_REG_R2, &x);
  /* 00000405.app：init 事件（evt==0）需 r3=上下文指针（INIT_CTX 零缓冲），
   * 否则 sub_8433C 解引用 r3+0x100 触发 MEM unmapped。其它事件 r3=y。 */
  uint32_t r3 = (evt == 0) ? INIT_CTX : y;
  uc_reg_write(g_uc, UC_ARM_REG_R3, &r3);

  uc_reg_write(g_uc, UC_ARM_REG_PC, &g_handler);
}

void on_touch_click(uint32_t x, uint32_t y) {
  log_info("触摸事件: (%u, %u) -> handler=0x%X instance=0x%X", x, y, g_handler,
           g_instance);
  /* 派发 case 9 (penDown)：记录按下点 */
  dispatch_applet_event(9, x, y);
  /* 排队 case 10 (penUp)：等 handler 执行完 case 9 后，下一轮事件循环再派发 */
  g_pending_touch.has_pending = true;
  g_pending_touch.evt = 10;
  g_pending_touch.x = x;
  g_pending_touch.y = y;
}

bool zm_event_dispatch_pending(void) {
  if (!g_pending_touch.has_pending)
    return false;
  dispatch_applet_event(g_pending_touch.evt, g_pending_touch.x,
                        g_pending_touch.y);
  g_pending_touch.has_pending = false;
  return true;
}

/* 无头模式：把第 n 次合成点击摊到 3x3 网格的格心上，
 * 尽量覆盖到 applet 的不同按钮，又不至于点到边缘外。 */
static void synth_click_pos(uint32_t n, uint32_t *px, uint32_t *py) {
  uint32_t w = g_header.ScreenW ? g_header.ScreenW : 240;
  uint32_t h = g_header.ScreenH ? g_header.ScreenH : 240;
  uint32_t col = n % 3;
  uint32_t row = (n / 3) % 3;
  *px = w * (2 * col + 1) / 6;
  *py = h * (2 * row + 1) / 6;
}

bool zm_event_loop_step(void) {
  g_event_rounds++;

  if (g_stop_requested)
    return false;

  if (g_max_events != 0 && g_event_rounds > g_max_events) {
    log_info("已达事件轮数上限 %u，结束事件循环", g_max_events);
    return false;
  }

  /* 1) 先补发排队中的 penUp，保证 tap 成对 */
  if (zm_event_dispatch_pending())
    return true;

  if (g_headless)
    zm_root_timer_tick();
  {
    uint32_t cb = 0, param = 0, ctx = 0;
    bool due = g_headless ? zm_root_timer_poll_virtual(g_uc, &cb, &param, &ctx)
                          : zm_root_timer_poll(g_uc, &cb, &param, &ctx);
    if (due) {
      uint32_t lr = TR_enter_event_loop;
      uc_reg_write(g_uc, UC_ARM_REG_LR, &lr);
      uc_reg_write(g_uc, UC_ARM_REG_R0, &param);
      /* 00000440 sub_30884 把 R1 当定时器对象 N 解引用；ctx 已在注册时
       * （trap.c TR_rt_timer）按回调门控捕获。其他 applet ctx=0 保持原行为。 */
      uc_reg_write(g_uc, UC_ARM_REG_R1, &ctx);
      uc_reg_write(g_uc, UC_ARM_REG_PC, &cb);
      return true;
    }
  }

  /* 2) 无头模式：
   *    - 00000405 的标签(instance+88)只在按键 a2==6(keycode∈{0..9})时由
   *      sub_827DC 填充(ROOT[0x140])——CREATE 后注入一次数字键，使红底黑字显示；
   *    - 有 -c N 时注入 N 次合成点击（每轮一个 penDown，下一轮跟 penUp）；
   *    - 没有 -c 时只是简单刷新（repaint）并把画布落盘，
   *      保证 -n N 能真正执行 N 轮，而不是一轮就结束。 */
  if (g_headless) {
    zm_gfx_present(); /* 把画布刷一次，便于截图落盘 */
    if (g_auto_clicks > 0) {
      g_auto_clicks--;
      uint32_t x = 0, y = 0;
      synth_click_pos(s_auto_click_idx++, &x, &y);
      on_touch_click(x, y);
      return true;
    }
    /* 没有合成点击时：若 applet 已注册定时器，就不再硬塞 repaint——
     * 0000050b 把 repaint(0x04) 当作"重新 SetTimer"钩子，每轮派发会把
     * 50ms 定时器反复重新计时而永不触发，动画死锁。
     * 没有定时器的 applet 仍派发 repaint 保底自绘。 */
    if (!zm_root_timer_has_active())
      dispatch_applet_event(ZMAEE_EV_REPAINT, 0, 0);
    return true;
  }

  /* 3) 有窗口：等真实事件 */
  return zm_gfx_event_loop(on_touch_click, g_hold_ms);
}

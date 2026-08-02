#include "./event.h"
#include "./log/log.h"

void dispatch_applet_event(uint32_t evt, uint32_t x, uint32_t y) {
  if (!g_instance || !g_handler)
    return;
  uint32_t sp = STACK_TOP;
  uint32_t lr = STACK_TOP;
  uc_reg_write(g_uc, UC_ARM_REG_SP, &sp);
  uc_reg_write(g_uc, UC_ARM_REG_LR, &lr);
  uc_reg_write(g_uc, UC_ARM_REG_R0, &g_instance);
  uc_reg_write(g_uc, UC_ARM_REG_R1, &evt);
  uc_reg_write(g_uc, UC_ARM_REG_R2, &x);
  /* 00000405.app：init 事件（evt==0）需 r3=上下文指针（INIT_CTX 零缓冲），
   * 否则 sub_8433C 解引用 r3+0x100 触发 MEM unmapped。其它事件 r3=y。 */
  uint32_t r3 = (evt == 0) ? INIT_CTX : y;
  uc_reg_write(g_uc, UC_ARM_REG_R3, &r3);
  uc_emu_start(g_uc, g_handler, STACK_TOP, 0, 0);
}

void on_touch_click(uint32_t x, uint32_t y) {
  log_info("触摸事件: (%u, %u) -> handler=0x%X instance=0x%X", x, y, g_handler,
           g_instance);
  dispatch_applet_event(9, x, y);
  dispatch_applet_event(10, x, y);
}

/*evt 跳转目标 含义 说明 0 sub_518 EVT_APP_START (init) ✅确认
 * 初始化：查接口、画 25 个按钮 1 sub_78C EVT_APP_STOP (cleanup) 推断 释放 init
 * 中获取的服务对象 2–8 default 未处理 直接返回 1（不关心的事件） 9 sub_824
 * EVT_PEN_DOWN ✅确认 记录按下点 10 sub_8B4 EVT_PEN_UP ✅确认 判定点击的按钮 →
 * 读 .zmr 播 MP3 11 sub_198 EVT_PEN_MOVE 推断 更新拖动包围盒 >11 default 未处理
 * 返回 1 */
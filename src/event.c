#include "./event.h"
#include "./log/log.h"
#include "./tool/uc_helper.h" /* uc_read32：调试用实例字段 dump */
#include "unicorn/arm.h"
#include <stdbool.h>

/* 待处理的触摸事件队列（仅支持 1 个，case 10 penUp 跟在 case 9 之后） */
static struct {
  bool has_pending;
  uint32_t evt;
  uint32_t x, y;
} g_pending_touch = {false, 0, 0, 0};

void dispatch_applet_event(uint32_t evt, uint32_t x, uint32_t y) {
  if (!g_instance || !g_handler)
    return;

  if (g_disasm)
    log_debug("派发事件 evt=%u (x=%u y=%u) -> handler=0x%X", evt, x, y,
              g_handler);

  uint32_t lr = TR_enter_event_loop;
  uc_reg_write(g_uc, UC_ARM_REG_LR, &lr);

  if (g_disasm) {
    /* dump 事件回调会用到的实例字段，便于定位"哪一个还没被初始化" */
    static const struct {
      uint32_t off;
      const char *name;
    } flds[] = {{0x10, "flag"},   {0x18, "a18"},   {0x34, "a34"},
                {0x3C, "a3C"},    {0x74, "a74"},   {0x80, "a80"},
                {0xA8, "row"},    {0xAC, "col"}};
    char buf[256];
    int p = 0;
    for (unsigned i = 0; i < sizeof(flds) / sizeof(flds[0]); i++)
      p += snprintf(buf + p, sizeof(buf) - (size_t)p, "%s(%#x)=%#x ",
                    flds[i].name, flds[i].off,
                    uc_read32(g_uc, g_instance + flds[i].off));
    log_debug("INSTANCE=0x%X 字段: %s", g_instance, buf);

    /* payload+0x180 是固件注入的"根对象指针"槽（见 emu.h ROOT_SLOT_OFF），
     * 而 sub_18B2C 会执行 `*(*(0x180))` 直接 BX 过去 —— 即它期望
     * *(ROOT) 是一个**可调用地址**（trap 或代码），不是对象/虚表。 */
    uint32_t slot = uc_read32(g_uc, BLOB_BASE + ROOT_SLOT_OFF);
    uint32_t fn = slot ? uc_read32(g_uc, slot) : 0;
    log_debug("payload[0x180]=0x%X  *(0x180)=0x%X  *(*0x180)=0x%X", BLOB_BASE,
              slot, fn);
  }

  uc_reg_write(g_uc, UC_ARM_REG_R0, &g_instance);
  uc_reg_write(g_uc, UC_ARM_REG_R1, &evt);
  uc_reg_write(g_uc, UC_ARM_REG_R2, &x);
  /* 00000405.app：init 事件（evt==0）需 r3=上下文指针（INIT_CTX 零缓冲），
   * 否则 sub_8433C 解引用 r3+0x100 触发 MEM unmapped。其它事件 r3=y。 */
  uint32_t r3 = (evt == 0) ? INIT_CTX : y;
  uc_reg_write(g_uc, UC_ARM_REG_R3, &r3);

  /* zmaee 的事件回调约定：**this 同时放在 R4**。
   *
   * 证据：applet 写到 API_SLOT+8 的"事件处理入口"常常是函数内部
   * 的标签而非函数起始（00000506 的 handler=0x1108C 即 sub_10E6C 内部
   * 的 loc_1108C），那段代码第一条就是 `mov r0, r4`
   * （拿 R4 当 this 用），而 sub_10E6C 的序言是 `push {r3-r5,lr}; mov r4, r0`。
   * 也就是说：固件按 (R0=this, R1=evt, R2=arg, R4=this) 调用它时，
   * 这些"跳过序言"的入口才能正常工作。R4 不设就会读到残留寄存器值，
   * 立刻在 sub_10B84 的 `ldr r0,[r4,#0x80]` 处访问野指针崩溃。 */
  uc_reg_write(g_uc, UC_ARM_REG_R4, &g_instance);

  uc_reg_write(g_uc, UC_ARM_REG_PC, &g_handler);
}

void on_touch_click(uint32_t x, uint32_t y) {
  log_info("触摸事件: (%u, %u) -> handler=0x%X instance=0x%X", x, y, g_handler,
           g_instance);
  /* 派发 case 9 (penDown)：记录按下点到 INSTANCE[25..26] */
  dispatch_applet_event(9, x, y);
  /* 排队 case 10 (penUp)：等 handler 执行完 case 9 后，下一轮事件循环再派发 */
  g_pending_touch.has_pending = true;
  g_pending_touch.evt = 10;
  g_pending_touch.x = x;
  g_pending_touch.y = y;
}

void on_touch_move(uint32_t x, uint32_t y) {
  /* 拖动：直接派发 evt=11（PEN_MOVE），不需要配对。
   * 移动事件很密集，不做日志以免刷屏。 */
  dispatch_applet_event(11, x, y);
}

bool zm_event_dispatch_pending(void) {
  if (!g_pending_touch.has_pending)
    return false;
  dispatch_applet_event(g_pending_touch.evt, g_pending_touch.x,
                        g_pending_touch.y);
  g_pending_touch.has_pending = false;
  return true;
}
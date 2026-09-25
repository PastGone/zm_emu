#include "./event.h"
#include "./log/log.h"
#include "./tool/uc_helper.h"       /* uc_read32：调试用实例字段 dump */
#include "./zmaee/audio/zm_audio.h" /* zm_audio_note_user_input：首次点击通知 */
#include "./test/zm_stat.h"        /* zm_stat_touch：点击坐标统计（ZM_STAT=1） */
#include "./zmaee/runtime/timer/zm_timer.h" /* zm_timer_async_call：触摸异步跳板 */
#include "unicorn/arm.h"
#include <stdbool.h>
#include <stdlib.h> /* getenv：ZM_NO_ASYNC_TOUCH 开关 */

/* 待处理的触摸事件队列（仅支持 1 个，case 10 penUp 跟在 case 9 之后） */
static struct {
  bool has_pending;
  uint32_t evt;
  uint32_t x, y;
} g_pending_touch = {false, 0, 0, 0};

/* applet 是否请求过关闭自己（IShell.CloseApplet） */
static bool s_close_requested = false;

/* ---- 触摸事件队列（自旋兜底路径）------------------------------------------
 *
 * 为什么需要：applet 有两种活法 ——
 *   ① 正常 yield：主循环里调 IShell.EnterEventLoop（宿主阻塞在
 *      zm_display_event_loop 里等事件），点击经那里的 SDL_PollEvent 直接派发；
 *   ② 自旋 / 0ms 定时器驱动的主循环（实测 0000048a《象棋新说》、00000442
 *      驱蚊大师）：它再也不回事件循环 → 点击**永远送不到**，用户看到的就是
 *      "点按钮没反应"（实测 0000048a：32 秒里 5 个点击一次都没派发）。
 * 所以：自旋兜底（zm_display_pump_events）把 SDL 里的点击**收进本队列**，
 * 由 hook 的异步通道（zm_event_async_poll）在 applet 长时间没 yield 时，
 * 用定时器同一条指令级跳板把 evt=9/10 送进去。 */
#define ZM_TOUCH_Q 8
static uint32_t s_q_x[ZM_TOUCH_Q], s_q_y[ZM_TOUCH_Q];
static int s_q_head = 0, s_q_n = 0;

void zm_event_queue_touch(uint32_t x, uint32_t y) {
  if (s_q_n >= ZM_TOUCH_Q) {
    log_warn("触摸队列已满（%d），丢弃本次点击 (%u,%u)", ZM_TOUCH_Q, x, y);
    return;
  }
  int i = (s_q_head + s_q_n) % ZM_TOUCH_Q;
  s_q_x[i] = x;
  s_q_y[i] = y;
  s_q_n++;
  /* 与 on_touch_click 保持一致：首次点击解除"进去默认关"的音频静音 +
   * 点击坐标统计（ZM_STAT=1）。 */
  zm_audio_note_user_input();
  zm_stat_touch(x, y);
}

bool zm_event_take_queued_touch(uint32_t *x, uint32_t *y) {
  if (s_q_n <= 0)
    return false;
  if (x)
    *x = s_q_x[s_q_head];
  if (y)
    *y = s_q_y[s_q_head];
  s_q_head = (s_q_head + 1) % ZM_TOUCH_Q;
  s_q_n--;
  return true;
}

bool zm_event_async_poll(uc_engine *uc, uint32_t resume_pc, bool starved) {
  static int disabled = -1;
  static uint32_t touch_idle = 0;
  if (disabled < 0) {
    const char *e = getenv("ZM_NO_ASYNC_TOUCH");
    disabled = (e && e[0] == '1') ? 1 : 0;
    /* 【触摸自己的饥饿门限】定时器那条是 300ms（打断 applet 有风险，慢一点没关系），
     * 但触摸是**用户手点** —— 用 300ms 的话手感就是"点了半秒才动"，实测反馈就是
     * "很卡"（applet 自身约 10~19 fps，再叠 300ms 就非常迟钝）。
     * 触摸在真机上本来就是**异步事件**（Java 事件线程随时回调 handler），所以这里
     * 用小得多的门限：默认 60ms，ZM_TOUCH_IDLE_MS 可调。 */
    const char *t = getenv("ZM_TOUCH_IDLE_MS");
    touch_idle = (t && atoi(t) > 0) ? (uint32_t)atoi(t) : 60u;
    if (disabled)
      log_info("触摸异步派发已禁用（ZM_NO_ASYNC_TOUCH=1）");
    else
      log_info("触摸异步派发：applet 连续 %ums 未回事件循环即送入（ZM_TOUCH_IDLE_MS 可调）",
               touch_idle);
  }
  if (disabled || !uc || !resume_pc || !g_handler || !g_instance)
    return false;
  /* 饥饿判定用触摸自己的门限（比定时器紧得多） */
  if (!starved && !zm_timer_is_starved_ms(uc, touch_idle))
    return false;

  uint32_t evt, x, y;
  if (g_pending_touch.has_pending) {
    /* 先补上一轮的 penUp：applet 的 tap 检测要成对的 case9 + case10 */
    evt = g_pending_touch.evt;
    x = g_pending_touch.x;
    y = g_pending_touch.y;
    g_pending_touch.has_pending = false;
  } else if (zm_event_take_queued_touch(&x, &y)) {
    evt = 9;
    g_pending_touch.has_pending = true;
    g_pending_touch.evt = 10;
    g_pending_touch.x = x;
    g_pending_touch.y = y;
  } else {
    return false;
  }

  /* 寄存器约定与 dispatch_applet_event 完全一致（R0=this, R1=evt, R2=x,
   * R3=y（evt==0 时是上下文指针）, R4=this）—— 区别只是改成经异步跳板送进去。 */
  uint32_t args[5] = {g_instance, evt, x, (evt == 0) ? INIT_CTX : y, g_instance};
  log_info("触摸异步派发: evt=%u (%u,%u) -> handler=0x%X（applet 长时间未回事件循环）",
           evt, x, y, g_handler);
  return zm_timer_async_call(uc, resume_pc, "触摸事件", g_handler, args, 5);
}

void zm_event_request_close(void) {
  if (s_close_requested)
    return;
  s_close_requested = true;
  log_info("IShell.CloseApplet：applet 请求关闭自己 → 先派发 EV_STOP(evt=1) "
           "让它收尾（停声音 + 存盘），收尾后结束模拟");
  dispatch_applet_event(1, 0, 0);
}

bool zm_event_close_requested(void) { return s_close_requested; }

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
    } flds[] = {{0x10, "flag"}, {0x18, "a18"}, {0x34, "a34"}, {0x3C, "a3C"},
                {0x74, "a74"},  {0x80, "a80"}, {0xA8, "row"}, {0xAC, "col"}};
    char buf[256];
    int p = 0;
    for (unsigned i = 0; i < sizeof(flds) / sizeof(flds[0]); i++)
      p += snprintf(buf + p, sizeof(buf) - (size_t)p, "%s(%#x)=%#x ",
                    flds[i].name, flds[i].off,
                    uc_read32(g_uc, g_instance + flds[i].off));
    log_debug("INSTANCE=0x%X 字段: %s", g_instance, buf);

    /* payload+0x180 是固件注入的"根对象指针"槽（见 emu.h ROOT_SLOT_OFF），
     * 而 sub_18B2C 会执行 `*(*(0x180))` 直接 BX 过去 —— 即它期望
     * *(ROOT_TABLE_ADDR) 是一个**可调用地址**（trap 或代码），不是对象/虚表。
     */
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
  /* 音频侧：通知"用户操作过了"（见 zm_audio.c 的 zm_audio_note_user_input /
   * sound_allowed）。注意：当前用的是"任意点击即解除进去默认关"这一版，
   * 与"只有点到声音开关才切换"的热区版（zm_audio_note_touch）二选一。 */
  zm_audio_note_user_input();
  /* 点击坐标统计（ZM_STAT=1）：找"哪块 UI 被反复点" */
  zm_stat_touch(x, y);
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
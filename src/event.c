#include "./event.h"
#include "./log/log.h"
#include "./tool/uc_helper.h"       /* uc_read32：调试用实例字段 dump */
#include "./zmaee/audio/zm_audio.h" /* zm_audio_note_user_input：首次点击通知 */
#include "./debug/zm_stat.h"       /* zm_stat_touch：点击坐标统计（ZM_STAT=1） */
#include "./zmaee/runtime/timer/zm_timer.h" /* zm_timer_async_call：触摸异步跳板 */
/* 键码真值在 event.h 的 ZMAEE_APPLET_INTERNAL_KEYCODE（见本文件末尾的映射说明）。
 * 注意：不要再 include zmaee/inc/zm_key_code.h —— 那份是旧的顺序枚举，
 * 枚举名与本文件用的重复 ⇒ 同时包含会**编译冲突** ✗。 */
#include <SDL2/SDL.h> /* SDL_Keycode / SDL_GetTicks（按键映射与长按计时） */
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
/* 队列元素是**完整事件**(evt + 两个参数)，不再只有坐标 —— 这样同一个队列既能装
 * 触摸(9/10)，也能装按键(5/6)等其它 APP_CMD，自旋兜底那条通道一并复用。 */
#define ZM_TOUCH_Q 8
static uint32_t s_q_evt[ZM_TOUCH_Q], s_q_x[ZM_TOUCH_Q], s_q_y[ZM_TOUCH_Q];
static int s_q_head = 0, s_q_n = 0;

void zm_event_queue(uint32_t evt, uint32_t x, uint32_t y) {
  if (s_q_n >= ZM_TOUCH_Q) {
    log_warn("事件队列已满（%d），丢弃 evt=%u (%u,%u)", ZM_TOUCH_Q, evt, x, y);
    return;
  }
  int i = (s_q_head + s_q_n) % ZM_TOUCH_Q;
  s_q_evt[i] = evt;
  s_q_x[i] = x;
  s_q_y[i] = y;
  s_q_n++;
}

/* 通用出队（evt/x/y 任一项可为 NULL）。 */
static bool take_queued(uint32_t *evt, uint32_t *x, uint32_t *y) {
  if (s_q_n <= 0)
    return false;
  int i = s_q_head;
  if (evt)
    *evt = s_q_evt[i];
  if (x)
    *x = s_q_x[i];
  if (y)
    *y = s_q_y[i];
  s_q_head = (s_q_head + 1) % ZM_TOUCH_Q;
  s_q_n--;
  return true;
}

bool zm_event_take_queued_evt(uint32_t *evt, uint32_t *x, uint32_t *y) {
  return take_queued(evt, x, y);
}

void zm_event_queue_touch(uint32_t x, uint32_t y) {
  zm_event_queue(APP_CMD_TOUCH_DOWN, x, y);
  /* 与 on_touch_click 保持一致：首次点击解除"进去默认关"的音频静音 +
   * 点击坐标统计（ZM_STAT=1）。 */
  zm_audio_note_user_input();
  zm_stat_touch(x, y);
}

bool zm_event_take_queued_touch(uint32_t *x, uint32_t *y) {
  /* 队列里可能混入按键事件（evt=5/6），而调用方（zm_display_event_loop）拿到坐标
   * 后一律走 on_click(x,y)。所以只取**第一个触摸按下**，其余（按键等）留在队列里
   * 由 take_queued 在别处消费。队列最多 8 项，直接搬迁即可。 */
  for (int k = 0; k < s_q_n; k++) {
    int i = (s_q_head + k) % ZM_TOUCH_Q;
    if (s_q_evt[i] != APP_CMD_TOUCH_DOWN)
      continue;
    if (x)
      *x = s_q_x[i];
    if (y)
      *y = s_q_y[i];
    for (int s = k; s < s_q_n - 1; s++) {
      int a = (s_q_head + s) % ZM_TOUCH_Q, b = (s_q_head + s + 1) % ZM_TOUCH_Q;
      s_q_evt[a] = s_q_evt[b];
      s_q_x[a] = s_q_x[b];
      s_q_y[a] = s_q_y[b];
    }
    s_q_n--;
    return true;
  }
  return false;
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
    /* 先补上一轮的 penUp：applet 的 tap 检测要成对的 按下 + 抬起 */
    evt = g_pending_touch.evt;
    x = g_pending_touch.x;
    y = g_pending_touch.y;
    g_pending_touch.has_pending = false;
  } else if (take_queued(&evt, &x, &y)) {
    /* ★ 通用队列：可能是触摸(9)，也可能是**按键**(5/6)等其它 APP_CMD
     * （队列已带事件码，见 zm_event_queue）。只有触摸需要"按下+抬起"配对
     * —— applet 的 tap 检测靠这一对；按键不需要。" */
    if (evt == APP_CMD_TOUCH_DOWN) {
      g_pending_touch.has_pending = true;
      g_pending_touch.evt = APP_CMD_TOUCH_UP;
      g_pending_touch.x = x;
      g_pending_touch.y = y;
    }
  } else {
    return false;
  }

  /* 寄存器约定与 dispatch_applet_event 完全一致（R0=this, R1=evt, R2=x,
   * R3=y（INIT 时是上下文指针）, R4=this）—— 区别只是改成经异步跳板送进去。 */
  uint32_t args[5] = {g_instance, evt, x,
                      (evt == APP_CMD_INIT) ? INIT_CTX : y, g_instance};
  log_info("异步派发: evt=%u (%u,%u) -> handler=0x%X（applet 长时间未回事件循环）",
           evt, x, y, g_handler);
  return zm_timer_async_call(uc, resume_pc, "applet 事件", g_handler, args, 5);
}

void zm_event_request_close(void) {
  if (s_close_requested)
    return;
  s_close_requested = true;
  log_info("IShell.CloseApplet：applet 请求关闭自己 → 先派发 DESTROY(evt=%u) "
           "让它收尾（停声音 + 存盘），收尾后结束模拟", APP_CMD_DESTROY);
  zm_event_send(APP_CMD_DESTROY, 0, 0);
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
  /* 00000405.app：init 事件需 r3=上下文指针（INIT_CTX 零缓冲），
   * 否则 sub_8433C 解引用 r3+0x100 触发 MEM unmapped。其它事件 r3=y。 */
  uint32_t r3 = (evt == APP_CMD_INIT) ? INIT_CTX : y;
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
  /* 派发 TOUCH_DOWN：记录按下点到 INSTANCE[25..26] */
  zm_event_send(APP_CMD_TOUCH_DOWN, x, y);
  /* 排队 TOUCH_UP：等 handler 执行完按下后，下一轮事件循环再派发
   * （applet 的 tap 检测要成对的下笔+抬笔） */
  g_pending_touch.has_pending = true;
  g_pending_touch.evt = APP_CMD_TOUCH_UP;
  g_pending_touch.x = x;
  g_pending_touch.y = y;
}

void on_touch_move(uint32_t x, uint32_t y) {
  /* 拖动：直接派发 TOUCH_MOVE，不需要配对。
   * 移动事件很密集，不做日志以免刷屏。 */
  zm_event_send(APP_CMD_TOUCH_MOVE, x, y);
}

bool zm_event_dispatch_pending(void) {
  if (!g_pending_touch.has_pending)
    return false;
  dispatch_applet_event(g_pending_touch.evt, g_pending_touch.x,
                        g_pending_touch.y);
  g_pending_touch.has_pending = false;
  return true;
}

/* ==================== APP_CMD 的语义封装 ====================
 * dispatch_applet_event 是"设寄存器"的原始通道（一次只能挂一个事件 ✗），
 * 下面是按语义包装过的对外实现，日志里能直接读出事件名。 */

static const char *evt_name(uint32_t cmd) {
  switch (cmd) {
  case APP_CMD_INIT: return "INIT";
  case APP_CMD_DESTROY: return "DESTROY";
  case APP_CMD_PAUSE: return "PAUSE";
  case APP_CMD_RESUME: return "RESUME";
  case APP_CMD_REPAINT: return "REPAINT";
  case APP_CMD_KEY_DOWN: return "KEY_DOWN";
  case APP_CMD_KEY_UP: return "KEY_UP";
  case APP_CMD_KEY_LONG_PRESS: return "KEY_LONG_PRESS";
  case APP_CMD_KEY_MULTIPLE: return "KEY_MULTIPLE";
  case APP_CMD_TOUCH_DOWN: return "TOUCH_DOWN";
  case APP_CMD_TOUCH_UP: return "TOUCH_UP";
  case APP_CMD_TOUCH_MOVE: return "TOUCH_MOVE";
  default: return "?";
  }
}

void zm_event_send(uint32_t cmd, uint32_t arg1, uint32_t arg2) {
  if (!g_instance || !g_handler) {
    log_debug("事件 %s(%u) 未派发：applet handler 还没建立", evt_name(cmd), cmd);
    return;
  }
  /* 按键类默认打 info：排查"按了没反应"时这是唯一证据；触摸/重绘太密走 debug。 */
  if (cmd >= APP_CMD_KEY_DOWN && cmd <= APP_CMD_KEY_MULTIPLE)
    log_info("派发 %s(%u) a1=%u -> handler=0x%X instance=0x%X", evt_name(cmd),
             cmd, arg1, g_handler, g_instance);
  else
    log_debug("派发 %s(%u) a1=%u a2=%u -> handler=0x%X", evt_name(cmd), cmd,
              arg1, arg2, g_handler);
  dispatch_applet_event(cmd, arg1, arg2);
}

void zm_event_pause(void) {
  zm_event_key_release_all(); /* 失焦时把按住的键松开，避免"卡键" */
  zm_event_send(APP_CMD_PAUSE, 0, 0);
}

void zm_event_resume(void) {
  /* 注意：这里不能紧接着再发 REPAINT —— dispatch 是"设寄存器"，两个事件连发
   * 第二个会把第一个覆盖掉（applet 只收得到最后一个）。重绘由 applet 自己的
   * Resume 分支负责；需要强制重绘时单独调 zm_event_repaint。 */
  zm_event_send(APP_CMD_RESUME, 0, 0);
}

void zm_event_repaint(void) { zm_event_send(APP_CMD_REPAINT, 0, 0); }

/* -------------------- 键盘：按下/抬起 + 长按(7) + 连发(8) --------------------
 *
 * 真机（功能机）的 Java/ZMAEE 应用就靠这四个事件码实现"按着不放连续输入"：
 *   KEY_DOWN → 长按(~600ms) → KEY_LONG_PRESS → 之后每 ~150ms 一个 KEY_MULTIPLE
 * 宿主的 SDL 键盘没有这些概念（只有 keydown/keyup + 系统级重复），所以这里维护
 * 一份"按住中"的键表，由事件循环周期性调用 zm_event_key_tick 补齐 7/8。 */

#define ZM_KEY_HELD 4
static struct {
  uint32_t code;
  uint32_t t0, tlast;
  int live, long_sent;
} s_held[ZM_KEY_HELD];

void zm_event_key(uint32_t keycode, int down) {
  if (down) {
    /* 系统/SDL 自带的重复按下会不断重发 keydown：忽略，连发由 tick 负责 */
    for (int i = 0; i < ZM_KEY_HELD; i++)
      if (s_held[i].live && s_held[i].code == keycode)
        return;
    int slot = -1;
    for (int i = 0; i < ZM_KEY_HELD; i++)
      if (!s_held[i].live) {
        slot = i;
        break;
      }
    if (slot < 0) {
      log_warn("按住的键超过 %d 个，忽略 keycode=%u", ZM_KEY_HELD, keycode);
      return;
    }
    uint32_t now = SDL_GetTicks();
    s_held[slot].code = keycode;
    s_held[slot].t0 = now;
    s_held[slot].tlast = now;
    s_held[slot].live = 1;
    s_held[slot].long_sent = 0;
    zm_event_send(APP_CMD_KEY_DOWN, keycode, 0);
    return;
  }
  for (int i = 0; i < ZM_KEY_HELD; i++)
    if (s_held[i].live && s_held[i].code == keycode) {
      s_held[i].live = 0;
      break;
    }
  zm_event_send(APP_CMD_KEY_UP, keycode, 0);
}

void zm_event_key_release_all(void) {
  for (int i = 0; i < ZM_KEY_HELD; i++) {
    if (!s_held[i].live)
      continue;
    uint32_t code = s_held[i].code;
    s_held[i].live = 0;
    zm_event_send(APP_CMD_KEY_UP, code, 0);
  }
}

bool zm_event_key_tick(uint32_t now_ms) {
  static int long_ms = -1, rep_ms = -1;
  if (long_ms < 0) {
    const char *e = getenv("ZM_KEY_LONG_MS");
    long_ms = (e && atoi(e) > 0) ? atoi(e) : 600;
    e = getenv("ZM_KEY_REPEAT_MS");
    rep_ms = (e && atoi(e) > 0) ? atoi(e) : 150;
  }
  /* 一轮只发一个：dispatch 是"设寄存器"，两个事件连发会互相覆盖 ✗ */
  for (int i = 0; i < ZM_KEY_HELD; i++) {
    if (!s_held[i].live)
      continue;
    if (!s_held[i].long_sent && now_ms - s_held[i].t0 >= (uint32_t)long_ms) {
      s_held[i].long_sent = 1;
      s_held[i].tlast = now_ms;
      zm_event_send(APP_CMD_KEY_LONG_PRESS, s_held[i].code, 0);
      return true;
    }
    if (s_held[i].long_sent && now_ms - s_held[i].tlast >= (uint32_t)rep_ms) {
      s_held[i].tlast = now_ms;
      zm_event_send(APP_CMD_KEY_MULTIPLE, s_held[i].code, 0);
      return true;
    }
  }
  return false;
}

/* -------------------- SDL 按键 → ZMAEE 内部键码 --------------------
 *
 * 【真值的来源】event.h 的 ZMAEE_APPLET_INTERNAL_KEYCODE（由 Android 键码推断），
 * 对应表见 docs/sdl 和内部键码的对应.md。
 *
 * ⚠ 早期那份 zm_key_code.h（顺序枚举的猜测值）已被删除，不要再用 ✗：
 *   与真机不符（例如 CENTER 真值 = 25，那里是 20；DPAD_UP 真值 = 13，那里是 16）。
 *   两份头的**枚举名相同** ⇒ 同时包含会编译冲突 ✗，只能留一份（留真值这份 ✓）。 */

bool zm_event_key_from_sdl(int sdl_sym, uint32_t *out_code) {
  static const struct {
    int sym;
    uint32_t code;
  } m[] = {
      /* 数字 0~9（小键盘同） */
      {SDLK_0, KEYCODE_0}, {SDLK_1, KEYCODE_1}, {SDLK_2, KEYCODE_2},
      {SDLK_3, KEYCODE_3}, {SDLK_4, KEYCODE_4}, {SDLK_5, KEYCODE_5},
      {SDLK_6, KEYCODE_6}, {SDLK_7, KEYCODE_7}, {SDLK_8, KEYCODE_8},
      {SDLK_9, KEYCODE_9},
      {SDLK_KP_0, KEYCODE_0}, {SDLK_KP_1, KEYCODE_1}, {SDLK_KP_2, KEYCODE_2},
      {SDLK_KP_3, KEYCODE_3}, {SDLK_KP_4, KEYCODE_4}, {SDLK_KP_5, KEYCODE_5},
      {SDLK_KP_6, KEYCODE_6}, {SDLK_KP_7, KEYCODE_7}, {SDLK_KP_8, KEYCODE_8},
      {SDLK_KP_9, KEYCODE_9},
      /* 方向：文档规定 W/A/S/D。另外把方向键也映射到同一组（PC 上更自然，
       * 文档没列但真机不会因此多收到任何"未定义"的键 —— 未列出的键一律忽略）。 */
      {SDLK_w, KEYCODE_DPAD_UP}, {SDLK_s, KEYCODE_DPAD_DOWN},
      {SDLK_a, KEYCODE_DPAD_LEFT}, {SDLK_d, KEYCODE_DPAD_RIGHT},
      {SDLK_UP, KEYCODE_DPAD_UP}, {SDLK_DOWN, KEYCODE_DPAD_DOWN},
      {SDLK_LEFT, KEYCODE_DPAD_LEFT}, {SDLK_RIGHT, KEYCODE_DPAD_RIGHT},
      /* 左确认(左软键) / 右返回(右软键)：Q / E；BACK 与 SOFT_RIGHT 同为 11 */
      {SDLK_q, KEYCODE_SOFT_LEFT}, {SDLK_e, KEYCODE_SOFT_RIGHT},
      {SDLK_ESCAPE, KEYCODE_BACK}, {SDLK_BACKSPACE, KEYCODE_BACK},
      {SDLK_AC_BACK, KEYCODE_BACK},
      /* 拨号 Z / 星号 N / 井号 M（PC 上的 * 与 # 键也一并映射） */
      {SDLK_z, KEYCODE_CALL},
      {SDLK_n, KEYCODE_STAR}, {SDLK_ASTERISK, KEYCODE_STAR},
      {SDLK_KP_MULTIPLY, KEYCODE_STAR},
      {SDLK_m, KEYCODE_POUND}, {SDLK_HASH, KEYCODE_POUND},
      /* 中心/确认：空格 / 回车 */
      {SDLK_SPACE, KEYCODE_CENTER}, {SDLK_RETURN, KEYCODE_CENTER},
      {SDLK_KP_ENTER, KEYCODE_CENTER},
      /* Home H / Search F */
      {SDLK_h, KEYCODE_HOME}, {SDLK_f, KEYCODE_SEARCH},
      /* 挂机键（DECall）：文档标"待定"，真机是否用得上未知 ⇒ **不映射**。
       * C/c 因此落到"未定义"，被忽略（不做想当然的猜测 ✗）。 */
  };
  for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++)
    if (m[i].sym == sdl_sym) {
      if (out_code)
        *out_code = m[i].code;
      return true;
    }
  return false;
}
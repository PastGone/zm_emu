/**
 * @file trap_handlers_root.c
 * @brief ROOT_TABLE_ADDR 那张根表的槽位 handler + 控制流类 handler
 *
 * 根表是 applet 与外部世界的第一个入口：malloc/free、字符串、数学、
 * create_cbk 等都在这里。控制流类（init 握手 / 事件循环 / abort /
 * 定时器返回跳板）也在本文件——它们同样挂在根表上。
 */

#include <stdlib.h> /* getenv / strtoul */
#include <string.h> /* strlen / memcpy */

#include "../emu.h"
#include "../event.h"                /* dispatch_applet_event / on_touch_click */
#include "../log/log.h"
#include "../tool/odds.h"            /* get_filename_from_fullpath */
#include "../tool/uc_helper.h"       /* uc_read32 / uc_write32 */
#include "../ulibc/ulibc.h"          /* u_sprintf / u_strtol / u_strtod_ex ... */
#include "../zmaee/core/zm_root.h"   /* zm_root_* / ZM_MATH_* */
#include "../zmaee/core/zm_str.h"    /* zm_str* / zm_wcslen / read_cstr */
#include "../zmaee/gfx/zm_display.h" /* zm_display_event_loop */
#include "../zmaee/runtime/timer/zm_timer.h" /* zm_timer_interrupt_return */
#include "trap_internal.h"

/* 把 double 结果按 ARM EABI 拆成 r0(低)/r1(高)。trap 框架只回写 r0，
 * 高 32 位需 handler 自己写 r1。 */
static uint32_t trap_ret_double(uc_engine *uc, double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  uint32_t hi = (uint32_t)(bits >> 32);
  uc_reg_write(uc, UC_ARM_REG_R1, &hi);
  return (uint32_t)(bits & 0xFFFFFFFFu);
}

/* ==========================================================================
 * 控制流类（表项 kind = TK_MANUAL_PC：自己写 PC / 停 emu）
 * ========================================================================== */

/* init 握手：把 SIZE_SLOT / API_SLOT 交给 applet，并把返回地址压成
 * 事件循环入口（applet 自己注册过就用它，否则用默认 TR_enter_event_loop）。 */
uint32_t a_reg_callback(trap_ctx *c) {
  uint32_t a0 = SIZE_SLOT;
  uint32_t a1 = API_SLOT;
  uc_reg_write(c->uc, UC_ARM_REG_R0, &a0);
  uc_reg_write(c->uc, UC_ARM_REG_R1, &a1);
  uint32_t callback_addr = g_registered_loop ? g_registered_loop : TR_enter_event_loop;
  uc_reg_write(c->uc, UC_ARM_REG_LR, &callback_addr);
  uint32_t entry = APPLET_ENTRY_POINT;
  uc_reg_write(c->uc, UC_ARM_REG_PC, &entry);
  log_info("init 握手：r0=&SIZE_SLOT、r1=&API_SLOT，返回地址=0x%X", callback_addr);
  return 0;
}

/* applet 通过 ROOT_TABLE_ADDR+0x1184 注册自己的事件循环入口 */
uint32_t a_register_event_loop(trap_ctx *c) {
  if (c->r0)
    g_registered_loop = c->r0;
  log_info("applet 注册事件循环入口: 0x%X", c->r0);
  return 0;
}

uint32_t a_abort(trap_ctx *c) {
  if (g_disasm) {
    char msg[64];
    read_cstr(c->uc, c->r0, msg, sizeof(msg));
    log_info("applet 调用 abort(\"%s\")，停止模拟", msg);
  } else {
    log_info("applet 调用 abort，停止模拟");
  }
  uc_emu_stop(c->uc);
  return 0;
}

/* 定时器异步中断的返回跳板：被打断的现场由 zm_timer.c 保存，
 * 回调执行完 `bx lr` 落到这里 → 恢复现场并回到被打断的那条指令。
 * 必须 TK_MANUAL_PC：PC 已由恢复逻辑写好，不能让分发器再写 R0/PC。 */
uint32_t a_timer_return(trap_ctx *c) {
  zm_timer_interrupt_return(c->uc);
  return 0;
}

/* 事件循环：一次进来干一件事（APP_CMD_INIT → APP_CMD_RESUME → SDL 泵 →
 * APP_CMD_DESTROY），每次都要回到 applet 的 handler，所以 PC 由分支自己派发。 */
uint32_t a_enter_event_loop(trap_ctx *c) {
  static int stage = 0;
  static int resume_enabled = 1;
  static int resume_inited = 0;
  uc_engine *uc = c->uc;

  if (zm_event_close_requested()) {
    log_info("applet 的退出回调已跑完（CloseApplet）→ 结束模拟");
    uc_emu_stop(uc);
    return 0;
  }
  if (!resume_inited) {
    const char *ar = getenv("ZM_AUTO_RESUME");
    resume_enabled = (!ar || ar[0] != '0');
    resume_inited = 1;
  }

  if (stage == 0) {
    uint32_t size = uc_read32(uc, SIZE_SLOT);
    uint32_t handler = uc_read32(uc, API_SLOT + 8);
    log_info("init 握手完成：size=%u handler=0x%X（API_SLOT=[%08X %08X %08X "
             "%08X]）",
             size, handler, uc_read32(uc, API_SLOT), uc_read32(uc, API_SLOT + 4),
             uc_read32(uc, API_SLOT + 8), uc_read32(uc, API_SLOT + 12));
    /* 宽松握手补丁：部分 applet（如 Hello World demo）往 SIZE_SLOT 写的
     * 是指针/自引用地址（如 0x7A8100，恰为该槽自身地址）而非整数实例大小，
     * 导致 size 为 0 或远超 0x40000 上限。只要 handler 有效就继续，size 改
     * 用兜底值——实例不可能太大，0x2000 足够且安全。 */
    if (handler == 0) {
      log_error("握手结果异常：handler=0，无法继续");
      uc_emu_stop(uc);
      return 0;
    }
    if (size == 0 || size > 0x40000) {
      const uint32_t FALLBACK_SIZE = 0x2000;
      log_warn("握手 size 异常（0x%X），按宽松握手使用兜底大小 %u 字节继续",
               size, FALLBACK_SIZE);
      size = FALLBACK_SIZE;
    }

    uint32_t blk = applet_calloc(uc, 1, size + 32);
    uint32_t INSTANCE = blk + 16;
    uc_write32(uc, blk + 0, uc_read32(uc, API_SLOT + 0));
    uc_write32(uc, blk + 4, uc_read32(uc, API_SLOT + 4));
    uc_write32(uc, blk + 8, uc_read32(uc, API_SLOT + 8));
    uc_write32(uc, INSTANCE, blk); /* +16 = self */
    const char *filename = get_filename_from_fullpath(g_app_pathname);
    size_t fn_len = strlen(filename) + 1;
    if (4 + fn_len <= (size_t)size + 32u)
      uc_mem_write(uc, INSTANCE + 4, filename, fn_len);
    else
      log_warn("filename (len=%zu) 超出实例大小 %u，跳过写入", fn_len, size);

    g_instance = INSTANCE;
    g_handler = handler;
    log_info("实例已分配：instance=0x%X（%u 字节），handler=0x%X", INSTANCE, size, handler);

    stage = 1;
    log_info("派发 APP_CMD_INIT -> handler=0x%X", g_handler);
    dispatch_applet_event(APP_CMD_INIT, 0, 0);
    log_info("APP_CMD_INIT 之后：访问 [CBK_OBJ+0x48] = 0x%X（我们初始化的是 0x%X）",
             uc_read32(uc, CBK_OBJ + 0x48), CBK_CTX);
    return 0;
  }

  if (stage == 1) {
    stage = 2;
    if (resume_enabled) {
      log_info("派发 APP_CMD_RESUME -> handler=0x%X", g_handler);
      dispatch_applet_event(APP_CMD_RESUME, 0, 0);
      return 0;
    }
  }

  uint32_t hold_ms = 0;
  const char *env = getenv("ZM_GFX_HOLD_MS");
  if (env && *env)
    hold_ms = (uint32_t)strtoul(env, NULL, 0);
  static int exit_dispatched = 0;
  if (exit_dispatched) {
    uc_emu_stop(uc);
    return 0;
  }
  if (!zm_display_event_loop(on_touch_click, hold_ms)) {
    const char *ex = getenv("ZM_EXIT_EVENT");
    if (ex && *ex && ex[0] != '0') {
      exit_dispatched = 1;
      log_info("派发 APP_CMD_DESTROY -> handler=0x%X（让 applet 自己收尾）", g_handler);
      dispatch_applet_event(APP_CMD_DESTROY, 0, 0);
      return 0;
    }
    uc_emu_stop(uc);
  }
  return 0;
}

/* ==========================================================================
 * 根表：对象 / 堆
 * ========================================================================== */

uint32_t a_root_getShell(trap_ctx *c) {
  (void)c;
  return G_SHELL_ADDR;
}

uint32_t a_root_malloc(trap_ctx *c) {
  uint32_t a_size = c->r0;
  uint32_t a_ctx = c->r1;
  uint32_t ret = applet_malloc(c->uc, c->r0);
  log_debug("malloc(size=%u ctx=0x%X lr=0x%X -> 0x%X", a_size, a_ctx, c->lr, ret);
  return ret;
}

uint32_t a_root_free(trap_ctx *c) {
  log_debug("free(0x%X) lr=0x%X", c->r0, c->lr);
  applet_free(c->uc, c->r0);
  return 0;
}

uint32_t a_root_malloc_screen(trap_ctx *c) {
  uint32_t ret = applet_malloc(c->uc, c->r0);
  log_debug("MallocScreenMem(0x%X) = 0x%X (lr=0x%X)", c->r0, ret, c->lr);
  return ret;
}

uint32_t a_root_free_screen(trap_ctx *c) {
  log_debug("FreeScreenMem(0x%X) lr=0x%X", c->r0, c->lr);
  applet_free(c->uc, c->r0);
  return 0;
}

uint32_t a_root_x3C(trap_ctx *c) {
  (void)c;
  return 0;
}

/* create_cbk：CBK 对象内嵌的"自管堆"要在返回前建好（见 trap_cbk_heap.c） */
uint32_t a_root_create_cbk(trap_ctx *c) {
  uint32_t ret = zm_root_create_cbk(c->uc);
  cbk_heap_init_once(c->uc);
  return ret;
}

uint32_t a_zm_root_cbk_default(trap_ctx *c) {
  return zm_root_cbk_default(c->uc, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_zm_root_x68C(trap_ctx *c) {
  return zm_root_x68C(c->uc, c->r0, c->r1, c->r2, c->r3);
}

/* ==========================================================================
 * 根表：字符串 / 内存 / 转换
 * ========================================================================== */

uint32_t a_zm_strcmp(trap_ctx *c) { return zm_strcmp(c->uc, c->r0, c->r1); }

uint32_t a_zm_strstr(trap_ctx *c) { return zm_strstr(c->uc, c->r0, c->r1); }

uint32_t a_zm_strchr(trap_ctx *c) { return zm_strchr(c->uc, c->r0, c->r1); }

uint32_t a_zm_strlen(trap_ctx *c) { return zm_strlen(c->uc, c->r0); }

uint32_t a_zm_wcslen(trap_ctx *c) { return zm_wcslen(c->uc, c->r0); }

uint32_t a_zm_spec_lookup(trap_ctx *c) { return zm_spec_lookup(c->uc, c->r0, c->r1); }

uint32_t a_zm_root_str_assign(trap_ctx *c) {
  return zm_root_str_assign(c->uc, c->r0, c->r1);
}

uint32_t a_zm_ucs2_to_utf8(trap_ctx *c) {
  return zm_ucs2_to_utf8(c->uc, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_zm_utf8_to_ucs2(trap_ctx *c) {
  return zm_utf8_to_ucs2(c->uc, c->r0, c->r1, c->r2, c->r3);
}

uint32_t a_root_str_ctor(trap_ctx *c) { return u_strcpy(c->uc, c->r0, c->r1); }

uint32_t a_root_str_to_num(trap_ctx *c) {
  return (uint32_t)u_strtol(c->uc, c->r0, c->r1, (int)c->r2);
}

uint32_t a_u_memcmp(trap_ctx *c) { return u_memcmp(c->uc, c->r0, c->r1, c->r2); }

uint32_t a_u_memcpy(trap_ctx *c) { return u_memcpy(c->uc, c->r0, c->r1, c->r2); }

uint32_t a_u_memset(trap_ctx *c) { return u_memset(c->uc, c->r0, c->r1, c->r2); }

uint32_t a_root_sprintf(trap_ctx *c) {
  u_va va;
  u_va_start_mem8(&va, c->uc, c->r2, 0);
  uint32_t ret = (uint32_t)u_sprintf(c->uc, c->r0, c->r1, &va);
  if (g_disasm) {
    char fmt[128], outp[256];
    uint32_t lr = 0;
    uc_reg_read(c->uc, UC_ARM_REG_LR, &lr);
    read_cstr(c->uc, c->r1, fmt, sizeof(fmt));
    read_cstr(c->uc, c->r0, outp, sizeof(outp));
    log_debug("sprintf lr=0x%X [%u参数]: fmt=\"%s\" out=\"%s\" ret=%u", lr,
              uc_read32(c->uc, c->r2), fmt, outp, ret);
  }
  return ret;
}

/* ==========================================================================
 * 根表：数学
 * ========================================================================== */

/* ROOT_TABLE[0x70] = 字符串→double（CString::ToDouble / atof）。
 * r0 指向以 0 结尾的 ASCII（CString 内联数据在 +0）。 */
uint32_t a_root_atof(trap_ctx *c) {
  double v = u_strtod_ex(c->uc, c->r0, NULL);
  if (getenv("ZM_SHOW_TRACE"))
    log_info("[atof] str@0x%X => %g", c->r0, v);
  return trap_ret_double(c->uc, v);
}

/* ROOT_TABLE[0x12C] = zmaee 的双精度二元运算（applet 侧包装是 sub_44E8）
 *
 * 调用约定是**标准 AAPCS**：
 *   r0:r1 = 左操作数 lhs（小端：r0 低 32 位、r1 高 32 位）
 *   r2:r3 = 右操作数 rhs
 *   [sp+0] = 运算符选择器：0=+ 1=- 2=× 3=÷（其余返回 0）
 *   返回 r0:r1 = 结果；**由调用方自己 STM 写回内存**，本 handler 不动内存。
 *
 * 【踩过的坑】最早写成"从栈上 sp+32 取操作数地址、再减 8 取另一个"，
 * 那个偏移只对 sub_10B4（四则运算）那个调用点碰巧成立；M+/M- 是另一处
 * 调用点（sub_1D9C 的 loc_25D8），sp+32 根本不是操作数地址，于是取到
 * lhs=rhs=0 —— 表现为"M+ 存不进去、MR 读出来永远是 0（像 MC）"。
 * 按 AAPCS 从寄存器取参后，两个调用点同时正确。
 *
 * 【顺序】必须是 lhs op rhs（先按的 op 后按的）：1-2 得 -1、1÷2 得 0.5。 */
uint32_t a_root_f_op(trap_ctx *c) {
  int op = (int)uc_read32(c->uc, c->sp + 0);
  uint64_t lb = ((uint64_t)c->r1 << 32) | (uint64_t)c->r0;
  uint64_t rb = ((uint64_t)c->r3 << 32) | (uint64_t)c->r2;
  double lhs = 0.0, rhs = 0.0, r = 0.0;
  memcpy(&lhs, &lb, sizeof(lhs));
  memcpy(&rhs, &rb, sizeof(rhs));
  switch (op) {
  case 0: r = lhs + rhs; break;
  case 1: r = lhs - rhs; break;
  case 2: r = lhs * rhs; break;
  case 3: r = (rhs != 0.0) ? lhs / rhs : 0.0; break;
  default: r = 0.0; break;
  }
  if (getenv("ZM_SHOW_TRACE"))
    log_info("[f_op] op=%d %g <op> %g => %g (lr=0x%X)", op, lhs, rhs, r, c->lr);
  return trap_ret_double(c->uc, r);
}

uint32_t a_root_srand(trap_ctx *c) {
  zm_root_srand(c->r0);
  return 0;
}

uint32_t a_root_rand(trap_ctx *c) {
  (void)c;
  return zm_root_rand();
}

uint32_t a_root_sqrt(trap_ctx *c) { return zm_root_math(c->uc, ZM_MATH_SQRT, c->r0, c->r1); }

uint32_t a_root_cos(trap_ctx *c) { return zm_root_math(c->uc, ZM_MATH_COS, c->r0, c->r1); }

uint32_t a_root_sin(trap_ctx *c) { return zm_root_math(c->uc, ZM_MATH_SIN, c->r0, c->r1); }

uint32_t a_root_atan(trap_ctx *c) { return zm_root_math(c->uc, ZM_MATH_ATAN, c->r0, c->r1); }

uint32_t a_root_tan(trap_ctx *c) { return zm_root_math(c->uc, ZM_MATH_TAN, c->r0, c->r1); }

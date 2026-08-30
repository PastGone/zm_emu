#include "../../include/u_stdlib.h"

/**
 * @file u_exit.c
 * @brief u_exit / u_abort / u_set_exit_handler —— 进程控制
 *
 * 三者围绕同一个回调指针，合并为一个翻译单元。
 */

static u_exit_fn s_exit_fn = NULL;

/** 由 std/stdlib/u_atexit.c 提供：逆序执行 atexit 回调 */
void u_atexit_run(uc_engine *uc);

static void default_exit(uc_engine *uc, int code) {
  (void)code;
  if (uc)
    uc_emu_stop(uc);
}

void u_set_exit_handler(u_exit_fn fn) { s_exit_fn = fn; }

void u_exit(uc_engine *uc, int code) {
  u_atexit_run(uc); /* 标准语义：exit 先跑完所有 atexit 回调 */
  if (s_exit_fn)
    s_exit_fn(uc, code);
  else
    default_exit(uc, code);
}

void u_abort(uc_engine *uc) { u_exit(uc, -1); }

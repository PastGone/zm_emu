#include "../../include/u_stdlib.h"

/**
 * @file u_atexit.c
 * @brief u_atexit —— 注册退出回调，在 u_exit 时按**逆序**调用
 *
 * 标准 atexit 不带参数；本变体允许带一个 void* 上下文，
 * 便于模拟器把 uc 之外的状态（如 applet 上下文）传进去。
 */

#define U_ATEXIT_MAX 32

typedef struct {
  void (*fn)(uc_engine *uc, void *arg);
  void *arg;
} u_atexit_slot;

static u_atexit_slot s_slots[U_ATEXIT_MAX];
static int s_count;

int u_atexit(void (*fn)(uc_engine *uc, void *arg), void *arg) {
  if (!fn || s_count >= U_ATEXIT_MAX)
    return -1;
  s_slots[s_count].fn = fn;
  s_slots[s_count].arg = arg;
  s_count++;
  return 0;
}

/** 由 u_exit 调用；非公开 */
void u_atexit_run(uc_engine *uc) {
  while (s_count > 0) {
    s_count--;
    void (*fn)(uc_engine *, void *) = s_slots[s_count].fn;
    void *arg = s_slots[s_count].arg;
    s_slots[s_count].fn = NULL;
    if (fn)
      fn(uc, arg);
  }
}

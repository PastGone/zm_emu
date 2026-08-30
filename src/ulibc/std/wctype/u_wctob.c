#include "../../include/u_wctype.h"

/**
 * @file u_wctob.c
 * @brief u_wctob —— 宽字符转单字节【纯透传】
 *
 * 与 u_btowc 同理：必须转发宿主 wctob。宿主的结果依赖当前 locale——
 * 在 UTF-8 等多字节 locale 下，U+0080..U+00FF 这些码点没有对应的
 * 单字节表示，标准规定返回 EOF。手写 `wc <= 0xFF ? wc : EOF`
 * 会在这些码点上与宿主不一致。
 */

int u_wctob(int wc) {
  if (wc == (int)WEOF)
    return EOF;
  return wctob((wint_t)wc);
}

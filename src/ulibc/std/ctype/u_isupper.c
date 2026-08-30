#include "../../include/u_ctype.h"

#include <ctype.h>

/**
 * @file u_isupper.c
 * @brief u_isupper —— 【纯透传】转发宿主 isupper
 *
 * 参数为 int（字符值）而非指针，不涉及客户机地址空间，可直接透传宿主。
 * 强转 unsigned char 是必须的：否则传入 EOF(-1) 等负值会让宿主实现产生 UB。
 */

int u_isupper(int c) { return isupper((unsigned char)c); }

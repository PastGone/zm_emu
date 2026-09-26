#include "../../include/u_ctype.h"

#include <ctype.h>

/**
 * @file u_isprint.c
 * @brief u_isprint —— 【纯透传】转发宿主 isprint
 *
 * 参数为 int（字符值）而非指针，不涉及客户机地址空间，可直接透传宿主。
 * 强转 unsigned char 是必须的：否则传入 EOF(-1) 等负值会让宿主实现产生 UB。
 */

int u_isprint(int c) {
	return isprint((unsigned char)c);
}

#include "../../include/u_wctype.h"

/**
 * @file u_towlower.c
 * @brief u_towlower —— 【纯透传】转发宿主 towlower（宽字符大小写转换）
 */

int u_towlower(int wc) {
	return (int)towlower((wint_t)wc);
}

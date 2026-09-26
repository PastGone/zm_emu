#include "../../include/u_wctype.h"

/**
 * @file u_iswcntrl.c
 * @brief u_iswcntrl —— 【纯透传】转发宿主 iswcntrl
 *
 * 参数为宽字符值而非指针，不涉及客户机地址空间。
 * BMP 内码点在宿主(UTF-32)与客户机(UCS-2)表示一致，可直接透传。
 */

int u_iswcntrl(int wc) {
	return iswcntrl((wint_t)wc);
}

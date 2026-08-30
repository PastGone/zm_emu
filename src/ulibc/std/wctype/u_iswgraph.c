#include "../../include/u_wctype.h"

/**
 * @file u_iswgraph.c
 * @brief u_iswgraph —— 【纯透传】转发宿主 iswgraph
 *
 * 参数为宽字符值而非指针，不涉及客户机地址空间。
 * BMP 内码点在宿主(UTF-32)与客户机(UCS-2)表示一致，可直接透传。
 */

int u_iswgraph(int wc) { return iswgraph((wint_t)wc); }

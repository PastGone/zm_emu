#include "../../include/u_wctype.h"

/**
 * @file u_towupper.c
 * @brief u_towupper —— 【纯透传】转发宿主 towupper（宽字符大小写转换）
 */

int u_towupper(int wc) { return (int)towupper((wint_t)wc); }

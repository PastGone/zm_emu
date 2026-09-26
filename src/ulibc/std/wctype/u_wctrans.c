#include "../../include/u_wctype.h"

#include <string.h>

/**
 * @file u_wctrans.c
 * @brief u_wctrans / u_towctrans —— 按名字取宽字符转换描述符
 */

enum { WTR_LOWER = 1, WTR_UPPER };

unsigned long u_wctrans(const char *name) {
	if (!name)
		return 0;
	if (strcmp(name, "tolower") == 0)
		return WTR_LOWER;
	if (strcmp(name, "toupper") == 0)
		return WTR_UPPER;
	return 0;
}

int u_towctrans(int wc, unsigned long desc) {
	switch (desc) {
	case WTR_LOWER:
		return u_towlower(wc);
	case WTR_UPPER:
		return u_towupper(wc);
	default:
		return wc;
	}
}

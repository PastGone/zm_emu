#include "../../include/u_wctype.h"

#include <string.h>

/**
 * @file u_wctype.c
 * @brief u_wctype / u_iswctype —— 按名字取宽字符分类描述符
 *
 * 描述符用 1..N 的小整数编码（宿主 wctype() 返回的是不透明类型，
 * 不能跨"接口边界"直接暴露给客户机），故自定义一套 C locale 编码。
 */

enum {
	WCT_ALNUM = 1,
	WCT_ALPHA,
	WCT_CNTRL,
	WCT_DIGIT,
	WCT_GRAPH,
	WCT_LOWER,
	WCT_PRINT,
	WCT_PUNCT,
	WCT_SPACE,
	WCT_UPPER,
	WCT_XDIGIT
};

unsigned long u_wctype(const char *name) {
	if (!name)
		return 0;
	static const struct {
		const char *n;
		unsigned long v;
	} tab[] = {{"alnum", WCT_ALNUM},
			   {"alpha", WCT_ALPHA},
			   {"cntrl", WCT_CNTRL},
			   {"digit", WCT_DIGIT},
			   {"graph", WCT_GRAPH},
			   {"lower", WCT_LOWER},
			   {"print", WCT_PRINT},
			   {"punct", WCT_PUNCT},
			   {"space", WCT_SPACE},
			   {"upper", WCT_UPPER},
			   {"xdigit", WCT_XDIGIT}};
	for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
		if (strcmp(name, tab[i].n) == 0)
			return tab[i].v;
	return 0;
}

int u_iswctype(int wc, unsigned long desc) {
	switch (desc) {
	case WCT_ALNUM:
		return u_iswalnum(wc);
	case WCT_ALPHA:
		return u_iswalpha(wc);
	case WCT_CNTRL:
		return u_iswcntrl(wc);
	case WCT_DIGIT:
		return u_iswdigit(wc);
	case WCT_GRAPH:
		return u_iswgraph(wc);
	case WCT_LOWER:
		return u_iswlower(wc);
	case WCT_PRINT:
		return u_iswprint(wc);
	case WCT_PUNCT:
		return u_iswpunct(wc);
	case WCT_SPACE:
		return u_iswspace(wc);
	case WCT_UPPER:
		return u_iswupper(wc);
	case WCT_XDIGIT:
		return u_iswxdigit(wc);
	default:
		return 0;
	}
}

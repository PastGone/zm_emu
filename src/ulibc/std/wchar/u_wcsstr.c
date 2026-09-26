#include "../../include/u_wcs.h"

#include <stdlib.h>
#include <wchar.h>

#include "../../include/u_mem.h"

/**
 * @file u_wcsstr.c
 * @brief u_wcsstr —— 宽串子串查找
 *
 * 短串（<=32K 宽字符）整块抓到宿主内存后用宿主 wcsstr；
 * 超长串退化为逐位比较。
 */

#define U_WFETCH_MAX 32768u

uint32_t u_wcsstr(uc_engine *uc, uint32_t hay, uint32_t needle) {
	if (!uc || hay == 0)
		return 0;
	uint32_t nlen = u_wcslen(uc, needle);
	if (nlen == 0)
		return hay;
	uint32_t hlen = u_wcslen(uc, hay);
	if (nlen > hlen)
		return 0;

	/* 整块抓到宿主内存后用宿主 wcsstr（宿主 wchar_t 为 4 字节，需逐个扩宽） */
	if (hlen <= U_WFETCH_MAX) {
		wchar_t *hs = (wchar_t *)malloc(((size_t)hlen + 1u) * sizeof(wchar_t));
		wchar_t *ns = (wchar_t *)malloc(((size_t)nlen + 1u) * sizeof(wchar_t));
		if (!hs || !ns) {
			free(hs);
			free(ns);
			return 0;
		}
		for (uint32_t i = 0; i <= hlen; i++)
			hs[i] = (wchar_t)u_rd16(uc, hay + i * 2u);
		for (uint32_t i = 0; i <= nlen; i++)
			ns[i] = (wchar_t)u_rd16(uc, needle + i * 2u);

		uint32_t r = 0;
		wchar_t *hit = wcsstr(hs, ns);
		if (hit)
			r = hay + (uint32_t)(hit - hs) * 2u;
		free(hs);
		free(ns);
		return r;
	}

	uint16_t first = u_rd16(uc, needle);
	uint32_t limit = hlen - nlen;
	for (uint32_t i = 0; i <= limit; i++) {
		if (u_rd16(uc, hay + i * 2u) != first)
			continue;
		uint32_t j = 0;
		for (; j < nlen; j++) {
			if (u_rd16(uc, hay + (i + j) * 2u) != u_rd16(uc, needle + j * 2u))
				break;
		}
		if (j == nlen)
			return hay + i * 2u;
	}
	return 0;
}

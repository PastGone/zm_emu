#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcstok.c
 * @brief u_wcstok / u_wcstok_reset —— 按分隔符集合切分宽串
 *
 * 与 u_strtok 同理：标准 wcstok 的游标是隐藏静态状态，
 * 客户机侧静态区难以约定，故保存在宿主侧。
 */

static uint32_t s_tok_addr;
static int s_tok_active;

void u_wcstok_reset(void) {
	s_tok_addr = 0;
	s_tok_active = 0;
}

/** 判断宽字符 c 是否在分隔符串 sep 中 */
static int is_sep(uc_engine *uc, uint32_t sep, uint16_t c) {
	uint32_t n = u_wcslen(uc, sep);
	for (uint32_t i = 0; i < n; i++) {
		if (u_rd16(uc, sep + i * 2u) == c)
			return 1;
	}
	return 0;
}

uint32_t u_wcstok(uc_engine *uc, uint32_t s, uint32_t sep) {
	if (!uc)
		return 0;

	uint32_t base;
	if (s != 0) {
		base = s;
		s_tok_active = 1;
	} else {
		if (!s_tok_active)
			return 0;
		base = s_tok_addr;
	}
	if (base == 0) {
		s_tok_active = 0;
		return 0;
	}

	/* 跳过前导分隔符 */
	for (;;) {
		uint16_t c = u_rd16(uc, base);
		if (c == 0) {
			s_tok_active = 0;
			s_tok_addr = 0;
			return 0;
		}
		if (!is_sep(uc, sep, c))
			break;
		base += 2u;
	}

	/* 找到分隔符终点 */
	uint32_t end = base;
	for (;;) {
		uint16_t c = u_rd16(uc, end);
		if (c == 0) {
			u_wr16(uc, end, 0);
			s_tok_addr = 0;
			s_tok_active = 0;
			return base;
		}
		if (is_sep(uc, sep, c)) {
			u_wr16(uc, end, 0);
			s_tok_addr = end + 2u;
			s_tok_active = 1;
			return base;
		}
		end += 2u;
	}
}

#include "../../include/u_str.h"

#include "../../include/internal/u_str_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_strcspn.c
 * @brief u_strcspn —— 返回首个出现在 reject 中字符之前的长度
 *
 * u_strpbrk 依赖本函数，而本函数不依赖 strpbrk。
 */

uint32_t u_strcspn(uc_engine *uc, uint32_t s, uint32_t reject) {
	if (!uc || s == 0)
		return 0;
	uint8_t tbl[32];
	u_str_build_set(uc, reject, tbl, sizeof(tbl));

	uint32_t len = u_strlen(uc, s);
	uint8_t buf[64];
	for (uint32_t off = 0; off < len;) {
		uint32_t c = len - off;
		if (c > (uint32_t)sizeof(buf))
			c = (uint32_t)sizeof(buf);
		if (!u_read(uc, s + off, buf, c))
			break;
		for (uint32_t i = 0; i < c; i++)
			if (u_str_set_has(tbl, buf[i]))
				return off + i;
		off += c;
	}
	return len;
}

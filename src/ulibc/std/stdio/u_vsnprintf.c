#include "../../include/u_fmt.h"

#include <stdlib.h>

/**
 * @file u_vsnprintf.c
 * @brief u_vsnprintf / u_snprintf —— 带长度限制的格式化
 *
 * snprintf 与 vsnprintf 语义完全相同，故同处一个翻译单元。
 */

int u_vsnprintf(uc_engine *uc, uint32_t dst, uint32_t size, uint32_t fmt, u_va *va) {
	if (!uc)
		return 0;

	char fmtbuf[U_FMT_MAX_FMT];
	u_read_cstr(uc, fmt, fmtbuf, sizeof(fmtbuf));

	size_t cap = size ? (size_t)size : 0;
	if (cap > U_FMT_MAX_OUT)
		cap = U_FMT_MAX_OUT;

	/* 宿主缓冲容量必须与客户机缓冲区容量**相等**（而不是 +1），
	 * 这样核心才会把内容截断到 cap-1 个字符并留出 '\0'，
	 * 与 snprintf 的语义完全一致（返回值仍是"本应写入"的长度）。 */
	char stack_buf[512];
	char *buf = stack_buf;
	size_t alloc = cap;
	if (alloc > sizeof(stack_buf)) {
		buf = (char *)malloc(alloc);
		if (!buf) {
			buf = stack_buf;
			alloc = sizeof(stack_buf);
		}
	}

	int n = u_vsnprintf_core(uc, va, buf, alloc, fmtbuf);

	if (dst && size && alloc > 0) {
		size_t w = (size_t)n + 1;
		if (w > cap)
			w = cap;
		if (w > alloc)
			w = alloc;
		u_write(uc, dst, buf, w);
	}
	if (buf != stack_buf)
		free(buf);
	return n;
}

int u_snprintf(uc_engine *uc, uint32_t dst, uint32_t size, uint32_t fmt, u_va *va) {
	return u_vsnprintf(uc, dst, size, fmt, va);
}

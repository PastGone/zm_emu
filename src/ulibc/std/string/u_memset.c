#include "../../include/u_mem.h"

#include <string.h>

/**
 * @file u_memset.c
 * @brief u_memset —— 按字节填充客户机内存（一级基本函数）
 */

uint32_t u_memset(uc_engine *uc, uint32_t dst, int c, uint32_t n) {
	if (!uc || n == 0)
		return dst;

	uint8_t buf[U_MEM_CHUNK];
	memset(buf, c & 0xFF, sizeof(buf));

	uint32_t done = 0;
	while (done < n) {
		uint32_t chunk = n - done;
		if (chunk > (uint32_t)sizeof(buf))
			chunk = (uint32_t)sizeof(buf);
		if (!u_write(uc, dst + done, buf, chunk))
			break;
		done += chunk;
	}
	return dst;
}

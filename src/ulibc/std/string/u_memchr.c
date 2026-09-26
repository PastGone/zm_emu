#include "../../include/u_mem.h"

/**
 * @file u_memchr.c
 * @brief u_memchr —— 在客户机内存块中查找字节，返回首次命中的客户机地址
 */

uint32_t u_memchr(uc_engine *uc, uint32_t src, int c, uint32_t n) {
	if (!uc || n == 0)
		return 0;

	uint8_t needle = (uint8_t)(c & 0xFF);
	uint8_t buf[U_MEM_CHUNK];
	uint32_t done = 0;
	while (done < n) {
		uint32_t chunk = n - done;
		if (chunk > (uint32_t)sizeof(buf))
			chunk = (uint32_t)sizeof(buf);
		if (!u_read(uc, src + done, buf, chunk))
			break;
		for (uint32_t i = 0; i < chunk; i++) {
			if (buf[i] == needle)
				return src + done + i;
		}
		done += chunk;
	}
	return 0;
}

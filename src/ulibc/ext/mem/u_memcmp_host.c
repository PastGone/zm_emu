#include "../../include/u_mem_ext.h"

#include <string.h>

#include "../../include/u_mem.h"

/**
 * @file u_memcmp_host.c
 * @brief u_memcmp_host —— 客户机缓冲区 vs 宿主缓冲区
 *
 * 非标准：模拟器便利函数，省去"先抓到宿主缓冲再比较"的样板代码。
 */

int u_memcmp_host(uc_engine *uc, uint32_t a, const void *b, uint32_t n) {
	if (!uc || !b || n == 0)
		return 0;

	uint8_t ba[U_MEM_CHUNK];
	const uint8_t *bb = (const uint8_t *)b;
	uint32_t done = 0;
	while (done < n) {
		uint32_t chunk = n - done;
		if (chunk > (uint32_t)sizeof(ba))
			chunk = (uint32_t)sizeof(ba);
		if (!u_read(uc, a + done, ba, chunk))
			return -1;
		int r = memcmp(ba, bb + done, chunk);
		if (r != 0)
			return r;
		done += chunk;
	}
	return 0;
}

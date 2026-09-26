#include "../../include/u_mem_ext.h"

#include "../../include/u_mem.h"

/**
 * @file u_memrchr.c
 * @brief u_memrchr —— 反向查找字节（GNU/POSIX 扩展，非 C 标准）
 *
 * 与 u_memchr 的关系：一个正向一个反向，各自独立，不互相依赖。
 */

uint32_t u_memrchr(uc_engine *uc, uint32_t src, int c, uint32_t n) {
	if (!uc || n == 0)
		return 0;

	uint8_t needle = (uint8_t)(c & 0xFF);
	uint8_t buf[U_MEM_CHUNK];
	uint32_t done = 0;
	while (done < n) {
		uint32_t chunk = n - done;
		if (chunk > (uint32_t)sizeof(buf))
			chunk = (uint32_t)sizeof(buf);
		uint32_t off = n - done - chunk;
		if (!u_read(uc, src + off, buf, chunk))
			break;
		uint32_t i = chunk;
		while (i > 0) {
			i--;
			if (buf[i] == needle)
				return src + off + i;
		}
		done += chunk;
	}
	return 0;
}

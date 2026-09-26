#include "../../include/u_mem.h"

/**
 * @file u_memcpy.c
 * @brief u_memcpy —— 客户机内存块复制（一级基本函数）
 */

uint32_t u_memcpy(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n) {
	if (!uc || n == 0)
		return 0;
	if (dst == src)
		return n;

	uint8_t buf[U_MEM_CHUNK];
	uint32_t done = 0;
	while (done < n) {
		uint32_t chunk = n - done;
		if (chunk > (uint32_t)sizeof(buf))
			chunk = (uint32_t)sizeof(buf);
		if (!u_read(uc, src + done, buf, chunk))
			break;
		if (!u_write(uc, dst + done, buf, chunk))
			break;
		done += chunk;
	}
	/* ★ 返回值必须与真机一致：参考里是 `void *zmaee_memcpy(void *a1, const void
	 * *a2, size_t a3)` —— 返回 **dst 指针**（libaee.so.c.txt:60803），不是拷贝长度！
	 * 实测 00000502：ROOT+0x5C 返回 0x14（长度）✗，而调用方是把它当 dst 用的
	 * （我们自己 trap.c 的注释也写着"返回值当 dst 用"），于是 applet 拼文件名时
	 * 前缀丢失、最终只打开 ".dat" ✗ —— 修好后它才能走到真正的资源加载。 */
	return done == n ? dst : dst;
}

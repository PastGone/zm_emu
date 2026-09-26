#include "../../include/u_stdlib.h"

#include <stdlib.h>

#include "../../include/u_mem.h"

/**
 * @file u_qsort_host.c
 * @brief u_qsort_host —— 排序（比较器在宿主侧）
 *
 * 标准 qsort 的比较器是客户机代码地址，需嵌套 uc_emu_start 回调；
 * 本变体接受宿主侧比较器。实现方式是整块抓到宿主内存 → qsort → 写回，
 * 比较器通过元素下标换算回客户机地址。
 */

typedef struct {
	uc_engine *uc;
	uint32_t guest_base;
	uint32_t width;
	u_compare_fn cmp;
	uint8_t *buf;
} u_sort_ctx;

static u_sort_ctx s_sort;

static int sort_wrap(const void *pa, const void *pb) {
	if (!s_sort.cmp)
		return 0;
	size_t ia = (size_t)((const uint8_t *)pa - s_sort.buf) / s_sort.width;
	size_t ib = (size_t)((const uint8_t *)pb - s_sort.buf) / s_sort.width;
	return s_sort.cmp(s_sort.uc,
					  s_sort.guest_base + (uint32_t)(ia * s_sort.width),
					  s_sort.guest_base + (uint32_t)(ib * s_sort.width));
}

void u_qsort_host(uc_engine *uc, uint32_t base, uint32_t n, uint32_t width, u_compare_fn cmp) {
	if (!uc || !cmp || n < 2 || width == 0 || base == 0)
		return;
	if (n > (0x40000000u / width))
		return; /* 防溢出 */

	size_t total = (size_t)n * width;
	uint8_t *buf = (uint8_t *)malloc(total);
	if (!buf)
		return;
	if (!u_read(uc, base, buf, total)) {
		free(buf);
		return;
	}

	s_sort.uc = uc;
	s_sort.guest_base = base;
	s_sort.width = width;
	s_sort.cmp = cmp;
	s_sort.buf = buf;

	qsort(buf, (size_t)n, (size_t)width, sort_wrap);
	u_write(uc, base, buf, total);
	free(buf);
}

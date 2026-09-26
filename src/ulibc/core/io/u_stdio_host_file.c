#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_stdio_host_file.c
 * @brief u_stdio_host_file —— 客户机句柄 → 宿主 FILE*
 */
FILE *u_stdio_host_file(uint32_t guest_fp) {
	return u_stdio_slot(guest_fp);
}

#include "../../include/u_time.h"

/**
 * @file u_difftime.c
 * @brief u_difftime —— 两个时间戳之差（秒）
 */

double u_difftime(int64_t end, int64_t begin) {
	return (double)(end - begin);
}

#include "../../include/u_str_ext.h"

#include "../../include/u_heap.h"

/**
 * @file u_strdup.c
 * @brief u_strdup —— 从客户机堆复制字符串（依赖 u_heap_strdup）
 *
 * POSIX 而非 C 标准；zmaee 会用到。
 */

uint32_t u_strdup(uc_engine *uc, uint32_t s) { return u_heap_strdup(uc, s); }

#include "../../include/u_stdlib.h"

/**
 * @file u_errno.c
 * @brief u_errno —— 客户机 errno 存储
 *
 * 宿主的 errno 会被模拟器自身（SDL、文件 IO 等）污染，
 * 客户机必须有一份独立的 errno。
 */

int u_errno = 0;

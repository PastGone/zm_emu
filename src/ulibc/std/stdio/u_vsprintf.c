#include "../../include/u_fmt.h"

/**
 * @file u_vsprintf.c
 * @brief u_vsprintf —— 无长度限制的变参格式化（依赖 u_sprintf）
 *
 * 与 u_sprintf 的唯一区别是**命名语义**（标准里 sprintf 是无长度限制的
 * 变参版本）。此处二者等价，内部同样按 U_FMT_MAX_OUT 防呆。
 */

int u_vsprintf(uc_engine *uc, uint32_t dst, uint32_t fmt, u_va *va) {
  return u_sprintf(uc, dst, fmt, va);
}

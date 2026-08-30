#include "../../include/u_stdlib.h"

/**
 * @file u_atof.c
 * @brief u_atof —— 简写转换（依赖 u_strtod_ex）
 */

double u_atof(uc_engine *uc, uint32_t s) { return u_strtod_ex(uc, s, NULL); }

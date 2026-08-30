#include "../../include/u_fmt_ext.h"

#include "../../include/u_arg.h"
#include "../../include/u_fmt.h"

/**
 * @file u_sprintf_u32.c
 * @brief u_sprintf_u32 —— 宿主数组参数的格式化（非标准便利接口）
 *
 * trap 处理器里常已把参数抓到宿主数组，用这个可省掉构造 u_va 的样板代码。
 */

int u_sprintf_u32(uc_engine *uc, uint32_t dst, uint32_t fmt,
                  const uint32_t *args, uint32_t nargs) {
  u_va va;
  u_va_start_array(&va, args, nargs);
  return u_sprintf(uc, dst, fmt, &va);
}

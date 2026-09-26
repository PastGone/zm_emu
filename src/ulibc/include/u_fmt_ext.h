#ifndef U_FMT_EXT_H
#define U_FMT_EXT_H
/**
 * @file u_fmt_ext.h
 * @brief 格式化**非标准**便利接口
 *
 * 标准 printf 家族要求参数是"客户机寄存器/栈上的变参"，通过 u_va 取。
 * 但 trap 处理器里经常已经把参数抓到了宿主数组中，或者只想一次性格式化，
 * 这时用下面两个包装能省掉构造 u_va 的样板代码。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

/**
 * 便利函数：参数已在宿主 uint32_t 数组里（老 trap 处理器最常用）。
 * args 里的元素按格式串中转换说明出现的顺序排列；
 * double / long long 占连续两个元素（小端：低 32 位在前）。
 */
int u_sprintf_u32(uc_engine *uc, uint32_t dst, uint32_t fmt, const uint32_t *args, uint32_t nargs);

#endif /* U_FMT_EXT_H */

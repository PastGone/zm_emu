#ifndef U_STR_EXT_IMPL_H
#define U_STR_EXT_IMPL_H
/**
 * @file u_str_ext_impl.h
 * @brief 字符串**非标准**扩展的内部共享实现
 *
 * stricmp/strnicmp 共用同一个大小写不敏感比较器；
 * strlwr/strupr 共用同一个原地大小写转换器。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

/**
 * @param n       当 bounded 非 0 时的比较上限
 * @param bounded 非 0 表示按 n 截断（strnicmp 语义）
 */
int u_icmp_impl(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n,
                int bounded);

/** 原地把客户机 C 串转成大写(upper!=0)或小写 */
uint32_t u_xlate_case(uc_engine *uc, uint32_t s, int upper);

#endif /* U_STR_EXT_IMPL_H */

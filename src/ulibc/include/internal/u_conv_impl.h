#ifndef U_CONV_IMPL_H
#define U_CONV_IMPL_H
/**
 * @file u_conv_impl.h
 * @brief 字符串→数值转换模块的内部共享扫描器
 *
 * strtol / strtoul / strtoll 各自是独立翻译单元，共用同一个
 * "扫前导空白→符号→进制前缀→数字"的状态机。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

/**
 * 扫描客户机 C 串中的无符号整数
 * @param end_out      接收结束位置（客户机地址）
 * @param neg_out      接收是否有负号
 * @param overflow_out 接收是否发生 64 位溢出
 * @return 累积值；无任何有效数字时返回 0（此时 end_out == s）
 */
uint64_t u_conv_scan_uint(uc_engine *uc, uint32_t s, int base,
                          uint32_t *end_out, int *neg_out, int *overflow_out);

#endif /* U_CONV_IMPL_H */

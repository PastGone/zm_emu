#ifndef U_STR_IMPL_H
#define U_STR_IMPL_H
/**
 * @file u_str_impl.h
 * @brief 字符串模块内部共享的字符集工具
 *
 * 供 strspn / strcspn / strpbrk / strtok 共用（它们各自是独立翻译单元）。
 * 对应 musl 的 src/internal/ 约定。
 */
#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/**
 * 把客户机 set 指向的 C 串展开成 256 位位图
 * @param tbl  至少 32 字节（256/8）
 */
void u_str_build_set(uc_engine *uc, uint32_t set, uint8_t *tbl, size_t tblsz);

/** 查询字符 c 是否在位图中 */
int u_str_set_has(const uint8_t *tbl, uint8_t c);

#endif /* U_STR_IMPL_H */

#ifndef U_STDIO_IMPL_H
#define U_STDIO_IMPL_H
/**
 * @file u_stdio_impl.h
 * @brief stdio 模块内部共享的客户机句柄表
 *
 * 客户机 FILE* 只是小整数句柄，宿主侧维护 FILE* 表。
 * fopen/fclose/fread/fwrite/... 各是独立翻译单元，共用这张表。
 */
#include <stdio.h>
#include <stdint.h>

/** 句柄表（定义于 std/stdio/u_stdio_tbl.c） */
extern FILE *u_stdio_g_files[];

/** 惰性绑定 stdin/stdout/stderr；可重复调用 */
void u_stdio_boot(void);

/** 取句柄对应的宿主 FILE*（含 bootstrap）；非法返回 NULL */
FILE *u_stdio_slot(uint32_t guest_fp);

/** 为 f 分配一个客户机句柄；表满则关闭 f 并返回 0 */
int u_stdio_alloc(FILE *f);

#endif /* U_STDIO_IMPL_H */

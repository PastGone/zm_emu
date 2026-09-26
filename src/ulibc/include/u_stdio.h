#ifndef U_STDIO_H
#define U_STDIO_H
/**
 * @file u_stdio.h
 * @brief 客户机 FILE* 桥接 —— 标准 C（stdio.h）【内存桥接类】
 *
 * 设计：客户机看到的 FILE* 只是一个**小整数句柄**（1/2/3 固定为
 * stdin/stdout/stderr），宿主侧维护一张 FILE* 表。
 * 这样 FILE 的内部结构体布局（ARM32 与 x86-64 完全不同）就彻底不是问题。
 *
 * 缓冲区的读写一律走「宿主临时缓冲 → uc_mem_read/uc_mem_write」，
 * 即核心逻辑复用宿主 libc，只做地址空间搬运。
 *
 * 【基建·非标准】u_stdio_host_file / u_stdio_cleanup 是模拟器专用。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

/** 客户机标准流句柄（1/2/3），与 newlib 的 stdin/stdout/stderr 编号一致 */
#define U_STDIN ((uint32_t)1)
#define U_STDOUT ((uint32_t)2)
#define U_STDERR ((uint32_t)3)

/** 同时可打开的文件数上限（含 3 个标准流） */
#define U_MAX_FILES 32

/* SEEK_* 在 ARM newlib 与 Linux 上同为 0/1/2，直接复用宿主常量 */

/* -------------------- 基建 -------------------- */

/** 客户机句柄 → 宿主 FILE*；非法句柄返回 NULL */
FILE *u_stdio_host_file(uint32_t guest_fp);

/** 关闭所有非标准流（模拟器复位时调用） */
void u_stdio_cleanup(void);

/* -------------------- 标准 C -------------------- */

/** 打开客户机路径指向的文件；失败返回 0 */
uint32_t u_fopen(uc_engine *uc, uint32_t path, uint32_t mode);

/** 关闭；返回 0 表示成功，EOF 表示失败（与标准一致） */
int u_fclose(uc_engine *uc, uint32_t fp);

/** 从 fp 读 n 个 size 字节的项到客户机 dst；返回成功读取的项数 */
uint32_t u_fread(uc_engine *uc, uint32_t dst, uint32_t size, uint32_t n, uint32_t fp);

/** 把客户机 src 的 n 个 size 字节写入 fp；返回成功写入的项数 */
uint32_t u_fwrite(uc_engine *uc, uint32_t src, uint32_t size, uint32_t n, uint32_t fp);

int u_fseek(uc_engine *uc, uint32_t fp, int32_t off, int whence);
int32_t u_ftell(uc_engine *uc, uint32_t fp);
void u_rewind(uc_engine *uc, uint32_t fp);
int u_feof(uc_engine *uc, uint32_t fp);
int u_fflush(uc_engine *uc, uint32_t fp);
int u_fgetc(uc_engine *uc, uint32_t fp);
int u_fputc(uc_engine *uc, uint32_t fp, int c);

/** 单字符读写（不触碰客户机内存） */
int u_getc(uc_engine *uc, uint32_t fp);
int u_getchar(uc_engine *uc); /* 固定读 stdin */
int u_putc(uc_engine *uc, uint32_t fp, int c);
int u_putchar(uc_engine *uc, int c); /* 固定写 stdout */

/** puts：把客户机 C 串写到 **stdout** 并追加 '\n'（与标准签名一致） */
int u_puts(uc_engine *uc, uint32_t s);

/** 把字符退回读缓冲区 */
int u_ungetc(uc_engine *uc, uint32_t fp, int c);

/** 错误标志：查询 / 清除 */
int u_ferror(uc_engine *uc, uint32_t fp);
void u_clearerr(uc_engine *uc, uint32_t fp);

/** perror：把 errno 描述（前缀为客户机串）写到 stderr */
void u_perror(uc_engine *uc, uint32_t s);

/** 从 fp 读一行（含 '\n'）到客户机 dst，最多 n-1 字符；EOF 且无数据返回 0 */
uint32_t u_fgets(uc_engine *uc, uint32_t dst, int n, uint32_t fp);

/** 把客户机 C 串写入 fp（不追加 '\n'） */
int u_fputs(uc_engine *uc, uint32_t s, uint32_t fp);

/** 删除 / 重命名文件（路径为客户机地址） */
int u_remove(uc_engine *uc, uint32_t path);
int u_rename(uc_engine *uc, uint32_t oldp, uint32_t newp);

#endif /* U_STDIO_H */

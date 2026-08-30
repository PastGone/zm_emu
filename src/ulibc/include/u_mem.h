#ifndef U_MEM_H
#define U_MEM_H
/**
 * @file u_mem.h
 * @brief 客户机（Unicorn 模拟内存）裸读写原语 + 标准 mem* 族
 *
 * 本文件混合了两类东西，已用注释隔开：
 *
 *  【基建·非标准】u_read / u_write / u_rd* / u_wr* / u_read_cstr /
 *                u_write_cstr —— 模拟器专用，标准 C 里没有对应物。
 *                这是整个 ulibc 的「第 0 层」，其余一切建立在它之上。
 *
 *  【标准 C】    u_memcpy / u_memmove / u_memset / u_memcmp / u_memchr /
 *                u_strlen —— C89 标准库函数。
 *                u_strnlen —— POSIX / C11（可选）。
 *
 * 非标准的 mem 扩展（memset16 / memset32 / memrchr / memcmp_host）
 * 放在独立头文件 u_mem_ext.h 里，实现在 ext/u_mem_ext.c。
 *
 * 设计要点：
 *  1. 客户机地址统一用 uint32_t 表示（ARM32）。
 *  2. 所有读写都校验 uc_mem_read / uc_mem_write 的返回码，
 *     访问未映射内存时安全失败（返回 0 / 已搬运字节数），绝不崩溃。
 *  3. 块操作内部按 U_MEM_CHUNK 分块，避免一次 malloc 巨大临时缓冲区。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/**
 * 块操作的分块大小：栈上缓冲区，避免每次调用都 malloc。
 * core/u_mem_block.c（标准 mem*）与 ext/u_mem_ext.c（非标准扩展）共用。
 */
#define U_MEM_CHUNK 512u

/* ====================【基建·非标准】裸读写原语 ==================== */

/** 从客户机 addr 读 len 字节到宿主 buf，全部成功返回 true */
bool u_read(uc_engine *uc, uint32_t addr, void *buf, size_t len);

/** 把宿主 buf 的 len 字节写入客户机 addr，全部成功返回 true */
bool u_write(uc_engine *uc, uint32_t addr, const void *buf, size_t len);

/** 读 / 写定长整数（小端，与客户机 ARM 默认端序一致）；失败返回 0 */
uint8_t u_rd8(uc_engine *uc, uint32_t addr);
uint16_t u_rd16(uc_engine *uc, uint32_t addr);
uint32_t u_rd32(uc_engine *uc, uint32_t addr);
void u_wr8(uc_engine *uc, uint32_t addr, uint8_t v);
void u_wr16(uc_engine *uc, uint32_t addr, uint16_t v);
void u_wr32(uc_engine *uc, uint32_t addr, uint32_t v);

/**
 * 把客户机 addr 处的 C 串读入宿主 buf（最多 cap-1 字符，保证 '\0' 结尾）
 * @return 写入 buf 的字符串长度（不含 '\0'）
 */
uint32_t u_read_cstr(uc_engine *uc, uint32_t addr, char *buf, size_t cap);

/**
 * 把宿主 C 串写入客户机 addr（含结尾 '\0'）
 * @return 实际写入字节数（含 '\0'）
 */
uint32_t u_write_cstr(uc_engine *uc, uint32_t addr, const char *s);

/* ====================【标准 C】string.h ==================== */

/** memcpy：返回实际搬运字节数（失败时可能小于 n） */
uint32_t u_memcpy(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n);

/** memmove：允许 src/dst 重叠 */
uint32_t u_memmove(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n);

/** memset：以字节 c 填充 n 字节，返回 dst */
uint32_t u_memset(uc_engine *uc, uint32_t dst, int c, uint32_t n);

/** memcmp：相等返回 0；a<b 返回负；a>b 返回正 */
int u_memcmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n);

/** 在客户机 src 起的 n 字节中查找字节 c，返回其客户机地址，未找到返回 0 */
uint32_t u_memchr(uc_engine *uc, uint32_t src, int c, uint32_t n);

/** 客户机 C 串长度（遇未映射内存即停止，不会死循环） */
uint32_t u_strlen(uc_engine *uc, uint32_t s);

/** 最多扫 maxlen 字节的 C 串长度（POSIX / C11） */
uint32_t u_strnlen(uc_engine *uc, uint32_t s, uint32_t maxlen);

#endif /* U_MEM_H */

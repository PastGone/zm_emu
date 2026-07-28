#ifndef __ZM_FS_H__
#define __ZM_FS_H__

// -------------------- fs / file 相关 trap 处理函数声明 --------------------
#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/**
 * @brief 载入 .zmr 资源文件，作为 file.read 的数据源
 *
 * 模块内部持有文件位置游标与数据缓冲，后续 file.read/seek 都基于此。
 * 多次调用会释放旧缓冲并载入新文件。
 *
 * @param path .zmr 文件路径
 * @return true 载入成功；false 载入失败（文件无法打开）
 */
bool zm_fs_load_zmr(const char *path);

/* fs.open：打开文件，返回文件对象地址（固定 FILE1），并重置游标 */
uint32_t zm_fs_open(uc_engine *uc);

/* file.close：关闭文件，固定返回 0 */
uint32_t zm_file_close(uc_engine *uc);

/**
 * @brief file.read：从当前游标读取 length 字节到客户机 buf
 * @param buf    目标客户机地址（对应 r1）
 * @param length 期望读取字节数（对应 r2）
 * @return 实际读取字节数（到达末尾则小于 length）
 */
uint32_t zm_file_read(uc_engine *uc, uint32_t buf, uint32_t length);

/**
 * @brief file.seek：移动文件游标
 * @param whence 0=绝对, 1=相对, 2=相对末尾（对应 r1）
 * @param offset 偏移量（对应 r2）
 * @return 固定返回 0
 */
uint32_t zm_file_seek(uc_engine *uc, uint32_t whence, uint32_t offset);

#endif

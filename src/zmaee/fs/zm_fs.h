#ifndef __ZM_FS_H__
#define __ZM_FS_H__

// -------------------- fs / file 相关 trap 处理函数声明 --------------------
// .zmr 资源文件格式：
//   0x00  4B  magic (0x30726D7A)
//   0x04  4B  资源个数 N
//   0x08  4B*(N+1)  偏移表（entry[i]=第 i 个资源起始绝对偏移，
//                   entry[N]=数据区末尾，即 CRC 前）
//   数据区  N 个资源块连续存放
//   末尾-4 4B  CRC32
//
// applet 的 sub_1DC 通过 fs.open/file.read/file.seek/file.close 读取资源；
// 本模块用单文件游标模拟这些 trap，同时解析偏移表供直接访问。
#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/**
 * @brief 载入并解析 .zmr 资源文件
 *
 * 解析 magic / 资源数 / 偏移表，作为 file.read 的数据源，
 * 同时供 zm_fs_get_resource 直接访问。
 *
 * @param path .zmr 文件路径
 * @return true 载入成功；false 载入失败（文件无法打开 / magic 错误 / 越界）
 */
bool zm_fs_load_zmr(const char *path);

/**
 * @brief 直接获取第 index 个资源（不经过 trap 游标）
 * @param index    资源索引（0..N-1）
 * @param out_size 输出资源字节数
 * @return 资源数据指针（指向内部缓冲，无需释放）；越界返回 NULL
 */
const uint8_t *zm_fs_get_resource(uint32_t index, uint32_t *out_size);

/* 资源个数 N */
uint32_t zm_fs_get_resource_count(void);

/* 释放 .zmr 缓冲与偏移表（进程退出前调用，可选） */
void zm_fs_shutdown(void);

/**
 * @brief fs.open：打开文件，返回文件对象地址（固定 FILE1），并重置游标
 *
 * 调用约定（见反编译 sub_1DC）：r0=this(FS), r1=filename, r2=mode。
 * 本实现只持有一个 .zmr，故忽略文件名匹配，仅记录便于调试。
 *
 * @param filename_ptr 文件名字符串客户机地址（对应 r1）
 * @return 文件对象地址（固定 FILE1）
 */
uint32_t zm_fs_open(uc_engine *uc, uint32_t filename_ptr);

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

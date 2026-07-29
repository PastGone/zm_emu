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

/* -------------------- 多文件 fs（00000405.app） --------------------
 * applet 通过 fs.open("%s%08x.app", path, id) / "config.b" / "zmsys006.dll"
 * 等打开多个文件。本层在 .zmr 单游标之外新增按名查表的多句柄表。
 * 句柄 = FILE1 + idx*0x10（idx 0..7）；applet 1 的 .zmr 路径在句柄未占用时
 * 自动回退（兼容）。
 */

#include <stdbool.h>

/**
 * @brief 登记一个按名可打开的文件（data 由调用方持有，不拷贝）
 * @param name basename（如 "00000405.app"），查找时按 basename 匹配
 * @param data 文件内容指针（需在进程生命周期内有效）
 * @param size 文件字节数
 * @return true 登记成功；false 表已满或参数非法
 */
bool zm_fs_register_file(const char *name, const void *data, size_t size);

/**
 * @brief 登记当前 applet 目录下默认可访问的文件
 *
 * 读入并在 fs 表中登记：app_list/config.b、app_list/zmsys006.dll、
 * app_list/zmsys001.dll、app_list/1_32icon.zbmp。
 * 失败的文件仅 log 跳过，不阻断。
 * @param applet_dir applet 所在目录（以 '/' 结尾或不含分隔符均可）
 */
void zm_fs_register_default(const char *applet_dir);

/**
 * @brief 从宿主磁盘读入一个文件并登记到 fs 表
 *
 * 缓冲由 fs 模块持有（s_owned），zm_fs_shutdown 时释放。
 * @param host_path 宿主文件路径
 * @param reg_name 登记用的 basename（如 "00000405.app"）
 * @return true 成功；false 文件不存在或读取失败
 */
bool zm_fs_register_hostfile(const char *host_path, const char *reg_name);

/**
 * @brief fs.open：打开文件，返回文件对象地址
 *
 * 调用约定（r0=this(FS), r1=filename, r2=mode）。
 * 解析 r1（兼容 zmaee 字符串对象与裸 C 串），取 basename 查表；
 * 命中则分配句柄 FILE1+idx*0x10；未命中且 .zmr 已载入则回退旧路径（applet 1）；
 * 否则返回 0。
 */
uint32_t zm_fs_open(uc_engine *uc, uint32_t filename_ptr);

/* file.close：关闭文件句柄，固定返回 0 */
uint32_t zm_file_close(uc_engine *uc, uint32_t file_id);

/**
 * @brief file.read：从当前游标读取 length 字节到客户机 buf
 * @param file_id 文件对象地址（对应 r0）
 * @param buf    目标客户机地址（对应 r1）
 * @param length 期望读取字节数（对应 r2）
 * @return 实际读取字节数（到达末尾则小于 length）
 */
uint32_t zm_file_read(uc_engine *uc, uint32_t file_id, uint32_t buf,
                      uint32_t length);

/**
 * @brief file.seek：移动文件游标
 * @param file_id 文件对象地址（对应 r0）
 * @param whence 0=绝对, 1=相对, 2=相对末尾（对应 r1）
 * @param offset 偏移量（对应 r2）
 * @return 固定返回 0
 */
uint32_t zm_file_seek(uc_engine *uc, uint32_t file_id, uint32_t whence,
                      uint32_t offset);

/**
 * @brief FILE_VT[0x24] file.size(file_id)：返回文件总大小
 *
 * sub_83F90 在 fs.open 后调用 (*FILE_VT[0x24])(handle) 取文件大小，
 * 与 0x50 比较判断是否有足够 header。本实现返回 slot 中登记的 size。
 * @param file_id 文件对象地址（对应 r0）
 * @return 文件字节数；未找到返回 0
 */
uint32_t zm_file_size(uc_engine *uc, uint32_t file_id);

/* FS_VT[+0x04] release：无操作返 0 */
uint32_t zm_fs_release(uc_engine *uc);

/* FS_VT[+0x14] chdir(str_obj)：stub，返 0（仅日志） */
uint32_t zm_fs_chdir(uc_engine *uc, uint32_t str_obj);

/* FS_VT[+0x30] enumFile(FS, index)：枚举目录下第 index 个文件。
 * sub_82584 用它遍历 app_list。stub 返回 0（无文件）使枚举循环立即退出。 */
uint32_t zm_fs_enum(uc_engine *uc, uint32_t fs_obj, uint32_t index);

#endif

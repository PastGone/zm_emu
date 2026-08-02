#ifndef ZM_FS_H
#define ZM_FS_H

#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h> /* uc_engine */

/* 虚拟文件句柄固定地址（需与实际内存布局一致） */

/* 设置文件搜索根目录（例如 applet 所在文件夹） */
void zm_fs_set_data_dir(const char *dir);

/* 打开文件：在数据目录下查找同名文件，读入内存，返回句柄 FILE1 */
uint32_t zm_fileMgr_open_file(uc_engine *uc, uint32_t filename_ptr);

/* 关闭当前打开的文件 */
uint32_t zm_file_close(uc_engine *uc, uint32_t file_id);

/* 读取文件内容到客户机内存 */
uint32_t zm_file_read(uc_engine *uc, uint32_t file_id, uint32_t buf,
                      uint32_t length);

/* 移动文件游标 */
uint32_t zm_file_seek(uc_engine *uc, uint32_t file_id, uint32_t whence,
                      uint32_t offset);

/* 获取文件大小 */
uint32_t zm_file_size(uc_engine *uc, uint32_t file_id);

/* 释放 FS 资源（空实现，保留接口兼容） */
uint32_t zm_fs_release(uc_engine *uc);

/* 默认初始化：设置数据目录（不扫描任何文件） */
void zm_fs_register_default(const char *applet_dir);

/* 释放所有资源 */
void zm_fs_shutdown(void);

#endif /* ZM_FS_H */
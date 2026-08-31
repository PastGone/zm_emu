#ifndef ZM_FILE_H
#define ZM_FILE_H

#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h> /* uc_engine */

/* ---------- IFile 层（单个已打开文件的读写游标） ----------
 * 负责：对当前由 FileMgr 打开的单个文件进行 close / read / seek / tell。
 * 全局单文件状态在此声明为 extern，供 FileMgr 的 open 在切换文件时清零。
 */

/* 单文件全局状态（定义在 zm_file.c）。FILE1 同时只持有一个打开的文件。 */
extern uint8_t *g_file_data; /* 当前打开文件的内容 */
extern size_t g_file_size;   /* 文件大小 */
extern uint32_t g_file_pos;  /* 读写游标 */

/* 关闭当前打开的文件 */
uint32_t zm_file_close(uc_engine *uc, uint32_t file_id);

/* 读取文件内容到客户机内存 */
uint32_t zm_file_read(uc_engine *uc, uint32_t file_id, uint32_t buf,
                      uint32_t length);

/* 移动文件游标 */
uint32_t zm_file_seek(uc_engine *uc, uint32_t file_id, uint32_t whence,
                      uint32_t offset);

/* IFile.Tell（vtable +0x24 = ZMAEE_IFile_Tell）：返回当前读写游标位置。
 * 取文件总大小的惯用法是 Seek(0, SEEK_END) 后调用本函数。 */
uint32_t zm_file_tell(uc_engine *uc, uint32_t file_id);

#endif /* ZM_FILE_H */

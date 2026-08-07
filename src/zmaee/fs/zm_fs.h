#ifndef ZM_FS_H
#define ZM_FS_H

#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h> /* uc_engine */

/* 同时可打开的文件数上限。每个句柄对应 FILE1 区内一个独立的对象，
 * 对象 word0 均指向 FILE_VT，因此 applet 可以像用真实对象一样调用它们。 */
#define ZM_MAX_FILES 8
#define ZM_FILE_OBJ_STRIDE 0x80U
/* 第 i 个文件句柄对象的客户机地址 */
#define ZM_FILE_OBJ(i) (FILE1 + (uint32_t)(i) * ZM_FILE_OBJ_STRIDE)

/* 设置文件读取根目录（applet 所在文件夹） */
void zm_fs_set_data_dir(const char *dir);

/* 设置写入根目录（沙箱）。applet 的写/新建/删除都落在这里，
 * 读取时优先查沙箱、其次查数据目录，避免污染原始测试素材。 */
void zm_fs_set_write_dir(const char *dir);

/* FileMgr_VT[0x08] open(name, mode) → 文件对象地址，失败 0 */
uint32_t zm_fileMgr_open_file(uc_engine *uc, uint32_t filename_ptr,
                              uint32_t mode);

/* FileMgr_VT[0x0C] remove(name) */
uint32_t zm_fileMgr_remove(uc_engine *uc, uint32_t name_ptr);
/* FileMgr_VT[0x10] rename(old, new) */
uint32_t zm_fileMgr_rename(uc_engine *uc, uint32_t old_ptr, uint32_t new_ptr);
/* FileMgr_VT[0x14] mkdir(name) */
uint32_t zm_fileMgr_mkdir(uc_engine *uc, uint32_t name_ptr);
/* FileMgr_VT[0x18] rmdir(name) */
uint32_t zm_fileMgr_rmdir(uc_engine *uc, uint32_t name_ptr);
/* FileMgr_VT[0x1C] exists(name) → 1/0 */
uint32_t zm_fileMgr_exists(uc_engine *uc, uint32_t name_ptr);
/* FileMgr_VT[0x20] stat(name) → 1=常规文件，0=目录/不存在 */
uint32_t zm_fileMgr_stat(uc_engine *uc, uint32_t name_ptr);
/* FileMgr_VT[0x2C] chdir(name) */
uint32_t zm_fileMgr_chdir(uc_engine *uc, uint32_t name_ptr);
/* FileMgr_VT[0x30] enum(dir, out) —— 目录枚举，stub 返回 0 项 */
uint32_t zm_fileMgr_enum(uc_engine *uc, uint32_t dir_ptr, uint32_t out_ptr);

/* FILE_VT[0x04] close/release */
uint32_t zm_file_close(uc_engine *uc, uint32_t file_id);
/* FILE_VT[0x08] read(buf, len) → 实际字节数 */
uint32_t zm_file_read(uc_engine *uc, uint32_t file_id, uint32_t buf,
                      uint32_t length);
/* FILE_VT[0x0C] write(buf, len) → 实际字节数 */
uint32_t zm_file_write(uc_engine *uc, uint32_t file_id, uint32_t buf,
                       uint32_t length);
/* FILE_VT[0x20] seek(whence, offset) */
uint32_t zm_file_seek(uc_engine *uc, uint32_t file_id, uint32_t whence,
                      uint32_t offset);
/* FILE_VT[0x24] size() */
uint32_t zm_file_size(uc_engine *uc, uint32_t file_id);
/* FILE_VT[0x28] tell() */
uint32_t zm_file_tell(uc_engine *uc, uint32_t file_id);

/* FileMgr_VT[0x04] release：关闭全部句柄 */
uint32_t zm_fs_release(uc_engine *uc);

/* 默认初始化：设置数据目录（不扫描任何文件） */
void zm_fs_register_default(const char *applet_dir);

/* 释放所有资源 */
void zm_fs_shutdown(void);

/* 把 applet 给出的名字解析成宿主机可读路径（供图片/资源加载复用）。
 * 依次尝试：写沙箱 → 当前工作子目录 → 数据目录 → 原样相对路径 → 递归两层。
 * 找到返回 true 并写入 out。 */
bool zm_fs_resolve_read(const char *name, char *out, size_t out_cap);

/* 统计：成功打开的文件数（供测试脚本判定"资源确实被读到了"） */
uint32_t zm_fs_open_success_count(void);

#endif /* ZM_FS_H */

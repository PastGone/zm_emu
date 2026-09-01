#ifndef ZM_FILE_MGR_H
#define ZM_FILE_MGR_H

#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h> /* uc_engine */

/* ---------- FileMgr 层（文件搜索目录 + 打开工厂） ----------
 * 负责：数据目录管理、按文件名打开（加载到内存）、释放、默认初始化。
 * 实际的单文件读写游标状态在 zm_file.h。
 */

/* 设置文件搜索根目录（例如 applet 所在文件夹） */
void zm_fs_set_data_dir(const char *dir);

/* 打开文件：在数据目录下查找同名文件，读入内存，返回句柄 FILE1。
 * 若已有文件打开，会自动先关闭前一个。 */
uint32_t zm_fileMgr_open_file(uc_engine *uc, uint32_t filename_ptr);

/* IFileMgr 通用 stub（g_filemgr_vtbl 未实现槽）：记录 offset 与参数，返回 0 */
uint32_t zm_fileMgr_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                         uint32_t r2, uint32_t r3);

/* +0x20 TestFile（RE sub_2A7BC）：文件存在性检查。
 * 存在 0 / 不存在 -1 / 参数空 -4。包内与 assets.zip 路径暂不支持。 */
uint32_t zm_fileMgr_TestFile(uc_engine *uc, uint32_t r0, uint32_t name_ptr);

/* +0x30 存储区支持查询（RE sub_29E40）：
 * a1(this)==0 → 0；a2==0 → 67('C' 内置盘)；a2==1 → 69('E')；
 * a2>=2 → SD 卡挂载则 84('T') 否则 0（模拟器恒视为已挂载）。 */
uint32_t zm_fileMgr_StorageSupport(uc_engine *uc, uint32_t r0, uint32_t type);

/* 释放 FS 资源（空实现，保留接口兼容） */
uint32_t zm_fs_release(uc_engine *uc);

/* 默认初始化：设置数据目录（不扫描任何文件） */
void zm_fs_register_default(const char *applet_dir);

/* 释放所有资源 */
void zm_fs_shutdown(void);

#endif /* ZM_FILE_MGR_H */

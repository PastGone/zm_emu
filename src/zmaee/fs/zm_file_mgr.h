#ifndef ZM_FILE_MGR_H
#define ZM_FILE_MGR_H

#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h> /* uc_engine */

/* ---------- G_FileMgr_ADDR 层（文件搜索目录 + 打开工厂） ----------
 * 负责：数据目录管理、按文件名打开（加载到内存）、释放、默认初始化。
 * 实际的单文件读写游标状态在 zm_file.h。
 */

/* 设置文件搜索根目录（例如 applet 所在文件夹） */
void zm_fs_set_data_dir(const char *dir);

/* 打开文件：在数据目录下查找同名文件，读入内存，返回句柄 FILE1。
 * 若已有文件打开，会自动先关闭前一个。 */
uint32_t zm_fileMgr_open_file(uc_engine *uc, uint32_t filename_ptr);

/* IFileMgr +0x14 = mkdir：确保目录存在（applet 在读写数据文件前会逐级调用）。
 * 参数同 OpenFile（r1 = guest 路径）。RE 依据见 zm_file_mgr.c 顶部注释。 */
uint32_t zm_fileMgr_make_dir(uc_engine *uc, uint32_t path_ptr);

/* IFileMgr 通用 stub（g_filemgr_vtbl 未实现槽）：记录 offset 与参数，返回 0 */
uint32_t zm_fileMgr_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                         uint32_t r2, uint32_t r3);

/* +0x20 TestFile（RE sub_2A7BC）：文件存在性检查。
 * 存在 0 / 不存在 -1 / 参数空 -4。包内与 assets.zip 路径暂不支持。 */
uint32_t zm_fileMgr_TestFile(uc_engine *uc, uint32_t r0, uint32_t name_ptr);

/* +0x0C = IFileMgr::GetInfo(mgr, name, out)，见 .c 里的实现注释。 */
uint32_t zm_fileMgr_GetInfo(uc_engine *uc, uint32_t r0, uint32_t name_ptr,
                            uint32_t out_ptr);

/* +0x30 存储区支持查询（RE sub_29E40）：
 * a1(this)==0 → 0；a2==0 → 67('C' 内置盘)；a2==1 → 69('E')；
 * a2>=2 → SD 卡挂载则 84('T') 否则 0（模拟器恒视为已挂载）。 */
uint32_t zm_fileMgr_StorageSupport(uc_engine *uc, uint32_t r0, uint32_t type);

/**
 * @brief IFileMgr+0x34 = GetFreeSize(this, drive_letter) → 剩余空间**字节数**
 *        （RE sub_29E08，libaee.so.c.txt:46929）
 *
 * 固件：非内置盘走 GetSDCardFreeSize、内置盘走 GetSystemMemSize，都是
 * "查到的 KB << 10" 再返回。宿主替身：statvfs 查数据目录所在分区真实可用量
 * （取不到退 128MB）。
 *
 * 00000442《驱蚊大师》启动时靠它判断"空间是否够 100K"，以前空桩返 0
 * → 必然弹"你的磁盘空间不足 100K"告警框。详见 zm_file_mgr.c 的注释。
 */
uint32_t zm_fileMgr_GetFreeSize(uc_engine *uc, uint32_t r0, uint32_t drive);

/* 释放 FS 资源（空实现，保留接口兼容） */
uint32_t zm_fs_release(uc_engine *uc);

/* 宿主侧整文件读取（供 IImage 等模块直接加载资源）：见下方实现注释 */

/* ---------- 宿主侧整文件读取（供 IImage 等模块直接加载资源） ----------
 * name：固件风格文件名（如 "res\\index_bg.jpg"，反斜杠/盘符由
 *       convert_file_name 归一化后映射到数据目录）。
 * 成功：返回 0，*out_buf 为 malloc 的缓冲区（调用方负责 free），
 *       *out_len 为字节数；失败：返回 -1，*out_buf=NULL、*out_len=0。 */
int zm_fs_read_file(const char *name, uint8_t **out_buf, size_t *out_len);

/* 把 IFile.Write 改过的内容写回宿主机文件（full_path 是 open 时解析出的全路径）。
 * 成功 0，失败 -1。 */
int zm_fs_write_back(const char *full_path, const uint8_t *data, size_t len);

/* 同上的"按 applet 给的名字"版本：走和 zm_fs_read_file 一样的归一化与目录搜索，
 * **文件不存在就创建**（新建存档就是这种情况），父目录不存在会逐级建。
 * 用途：CBK 文件对象（zm_cbk_file.c）关闭时落盘 —— 没有它，那一族的
 * "存档/设置"写进内存就丢了 ✗。成功 0，失败 -1。 */
int zm_fs_write_back_name(const char *name, const uint8_t *data, size_t len);

/* 默认初始化：设置数据目录（不扫描任何文件） */
void zm_fs_register_default(const char *applet_dir);

/* 释放所有资源 */
void zm_fs_shutdown(void);

#endif /* ZM_FILE_MGR_H */

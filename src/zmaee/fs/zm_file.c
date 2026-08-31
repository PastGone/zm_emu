#include "zm_file.h"

#include <stdlib.h>
#include <string.h>

#include "../../emu.h"
#include "../../log/log.h"

/* ========== 通用单文件系统：IFile 层 ==========
 *
 * 对当前由 FileMgr 打开的单个文件（g_file_data / g_file_size / g_file_pos）
 * 进行 close / read / seek / tell。同一时刻只有 FILE1 一个打开的文件。
 */

/* ========== 单文件全局状态 ========== */
uint8_t *g_file_data = NULL; /* 当前打开文件的内容 */
size_t g_file_size = 0;      /* 文件大小 */
uint32_t g_file_pos = 0;     /* 读写游标 */

uint32_t zm_file_close(uc_engine *uc, uint32_t file_id) {
  (void)uc;
  if (file_id == FILE1 && g_file_data) {
    free(g_file_data);
    g_file_data = NULL;
    g_file_size = 0;
    g_file_pos = 0;
    log_debug("file.close");
  }
  return 0;
}

uint32_t zm_file_read(uc_engine *uc, uint32_t file_id, uint32_t buf,
                      uint32_t length) {
  if (file_id != FILE1 || !g_file_data)
    return 0;

  uint32_t pos = g_file_pos;
  uint32_t remain = (g_file_size >= pos) ? (uint32_t)(g_file_size - pos) : 0;
  uint32_t n = (length < remain) ? length : remain;
  if (n > 0) {
    uc_mem_write(uc, buf, g_file_data + pos, n);
    g_file_pos += n;
  }
  return n;
}

uint32_t zm_file_seek(uc_engine *uc, uint32_t file_id, uint32_t whence,
                      uint32_t offset) {
  (void)uc;
  if (file_id != FILE1 || !g_file_data)
    return 0;

  if (whence == 0)
    g_file_pos = offset;
  else if (whence == 1)
    g_file_pos += offset;
  else if (whence == 2)
    g_file_pos = (uint32_t)g_file_size + offset;
  return 0;
}

uint32_t zm_file_tell(uc_engine *uc, uint32_t file_id) {
  (void)uc;
  /*
   * IFile vtable +0x24 是 ZMAEE_IFile_Tell（逆向实测），返回**当前读写位置**。
   * 之前实现为"返回总大小"，只在 Seek(0,SEEK_END) 之后调用才碰巧正确。
   * 取总大小请走 Seek(0, SEEK_END) + Tell() 这个惯用法。
   */
  if (file_id == FILE1 && g_file_data)
    return g_file_pos;
  return 0;
}

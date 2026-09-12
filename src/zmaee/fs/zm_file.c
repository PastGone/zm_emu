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
  /* 观测：applet 到底有没有通过 IFile.Read 取文件内容。
   * 前 20 次逐条打印，之后每 500 次汇总一次（避免刷屏）。 */
  {
    static uint32_t nread = 0, nfail = 0, total = 0;
    nread++;
    total += n;
    if (n == 0)
      nfail++;
    if (nread <= 20)
      log_info("file.read(#%u) buf=0x%X len=%u -> %u (pos=%u size=%zu)", nread, buf,
               length, n, pos, g_file_size);
    else if ((nread % 500) == 0)
      log_info("file.read 累计 %u 次, 累计取回 %u 字节, 空读 %u 次", nread, total,
               nfail);
  }
  return n;
}

uint32_t zm_file_seek(uc_engine *uc, uint32_t file_id, uint32_t whence,
                      uint32_t offset) {
  (void)uc;
  if (file_id != FILE1 || !g_file_data)
    return 0;

  /*
   * IFile vtable +0x20 = ZMAEE_IFile_Seek(ifile, nWhence, nOffset)。
   * whence 语义（由 applet 反编译代码实测确认）：
   *   0 = 从文件头开始（绝对偏移）
   *   1 = 从文件尾开始（offset 为负表示向前），applet 用 Seek(f,1,0)+Tell(f) 取文件大小
   *   2 = 从当前位置开始
   * 例如 theme/00000002 中：Seek(f,1,0); size=Tell(f); Seek(f,0,12); Read(...)。
   */
  int32_t off = (int32_t)offset;
  int64_t pos;
  if (whence == 0)
    pos = (int64_t)off;
  else if (whence == 1)
    pos = (int64_t)g_file_size + off;
  else
    pos = (int64_t)g_file_pos + off;

  if (pos < 0)
    pos = 0;
  if (pos > (int64_t)g_file_size)
    pos = (int64_t)g_file_size;
  g_file_pos = (uint32_t)pos;
  return 0;
}

uint32_t zm_file_tell(uc_engine *uc, uint32_t file_id) {
  (void)uc;
  /*
   * IFile vtable +0x24 是 ZMAEE_IFile_Tell（逆向实测），返回**当前读写位置**。
   * applet 取文件大小的惯用法是 Seek(f,1,0) 跳到文件尾后再 Tell()。
   */
  if (file_id == FILE1 && g_file_data)
    return g_file_pos;
  return 0;
}

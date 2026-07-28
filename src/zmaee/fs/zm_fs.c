#include "zm_fs.h"

#include <stdio.h>
#include <stdlib.h>

#include "../core/zm_addrs.h"

/* ---------- 模块内部状态 ---------- */
/* .zmr 资源数据缓冲与游标，由 zm_fs_load_zmr 初始化 */
static uint8_t *s_zmr_data = NULL;
static size_t s_zmr_size = 0;
static uint32_t s_file_pos = 0;

bool zm_fs_load_zmr(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    return false;
  }
  uint8_t *buf = malloc((size_t)sz);
  if (!buf) {
    fclose(f);
    return false;
  }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz) {
    free(buf);
    return false;
  }

  /* 释放旧缓冲，替换为新数据 */
  free(s_zmr_data);
  s_zmr_data = buf;
  s_zmr_size = (size_t)sz;
  s_file_pos = 0;
  return true;
}

/* fs.open：返回文件对象地址（固定 FILE1），并重置游标 */
uint32_t zm_fs_open(uc_engine *uc) {
  (void)uc;
  s_file_pos = 0;
  return FILE1;
}

/* file.close：固定返回 0 */
uint32_t zm_file_close(uc_engine *uc) {
  (void)uc;
  return 0;
}

/* file.read：从当前游标读取 length 字节到客户机 buf */
uint32_t zm_file_read(uc_engine *uc, uint32_t buf, uint32_t length) {
  uint32_t pos = s_file_pos;
  /* 剩余可读字节数（防止 size - pos 在 pos 越界时下溢） */
  uint32_t remain =
      (s_zmr_size >= pos) ? (uint32_t)(s_zmr_size - pos) : 0;
  uint32_t n = (length < remain) ? length : remain;
  if (n > 0) {
    uc_mem_write(uc, buf, s_zmr_data + pos, n);
    s_file_pos += n;
  }
  return n;
}

/* file.seek：移动文件游标
 * whence: 0=绝对, 1=相对当前, 2=相对末尾 */
uint32_t zm_file_seek(uc_engine *uc, uint32_t whence, uint32_t offset) {
  (void)uc;
  if (whence == 0) {
    s_file_pos = offset;
  } else if (whence == 1) {
    s_file_pos += offset;
  } else if (whence == 2) {
    s_file_pos = (uint32_t)s_zmr_size + offset;
  }
  return 0;
}

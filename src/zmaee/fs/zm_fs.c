#include "zm_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../emu.h"
#include "../../log/log.h"
#include "../core/zm_str.h" /* read_cstr */

/* ---------- 通用单文件系统（无槽位、无注册、无 .zmr 绑定） ----------
 *
 *  - 只同时支持一个打开的文件
 *  - fs.open 会在宿主机磁盘上找到同名文件并读入内存，返回固定句柄 FILE1
 *  - 若再次调用 fs.open，会自动关闭前一个文件并打开新文件
 *  - 不需要预先注册任何文件
 *
 * 使用方式：
 *   1. 调用 zm_fs_set_data_dir("/path/to/applet") 设置文件搜索根目录
 *   2. applet 调用 fs.open("xxx") 时，会尝试打开 "/path/to/applet/xxx"
 *   3. 调用 fs.close 或 shutdown 时释放内存
 */

/* ========== 全局单文件状态 ========== */
static uint8_t *g_file_data = NULL; // 当前打开文件的内容
static size_t g_file_size = 0;      // 文件大小
static uint32_t g_file_pos = 0;     // 读写游标
static char s_data_dir[1024] = {0}; // 文件查找根目录

/* ========== 内部工具 ========== */
static void basename_of(const char *path, char *out, size_t out_cap) {
  const char *base = path;
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\')
      base = p + 1;
  }
  size_t n = strlen(base);
  if (n > out_cap - 1)
    n = out_cap - 1;
  memcpy(out, base, n);
  out[n] = '\0';
}

static void read_filename(uc_engine *uc, uint32_t ptr, char *buf, size_t cap) {
  zm_read_str_obj(uc, ptr, buf, cap);
}

/* ========== 对外 API ========== */

/**
 * 设置文件搜索根目录（通常为 applet 所在文件夹）。
 * 需要在第一次 fs.open 之前调用。
 */
void zm_fs_set_data_dir(const char *dir) {
  if (dir && dir[0]) {
    strncpy(s_data_dir, dir, sizeof(s_data_dir) - 1);
    s_data_dir[sizeof(s_data_dir) - 1] = '\0';
    /* 去掉末尾斜杠 */
    size_t len = strlen(s_data_dir);
    while (len > 0 &&
           (s_data_dir[len - 1] == '/' || s_data_dir[len - 1] == '\\'))
      s_data_dir[--len] = '\0';
  } else {
    s_data_dir[0] = '\0';
  }
}

/**
 * 打开文件 —— 自动关闭前一个文件，加载新文件到内存，返回 FILE1。
 * 成功返回 FILE1，失败返回 0。
 */
uint32_t zm_fileMgr_open_file(uc_engine *uc, uint32_t filename_ptr) {
  char name[256];
  read_filename(uc, filename_ptr, name, sizeof(name));
  char bn[64];
  basename_of(name, bn, sizeof(bn));

  /* 先关闭之前打开的文件 */
  if (g_file_data) {
    free(g_file_data);
    g_file_data = NULL;
    g_file_size = 0;
    g_file_pos = 0;
  }

  if (s_data_dir[0] == '\0') {
    log_error("zm_fs_open: 未设置数据目录，无法打开 \"%s\"", bn);
    return 0;
  }

  char full_path[1280];
  snprintf(full_path, sizeof(full_path), "%s/%s", s_data_dir, bn);

  FILE *fp = fopen(full_path, "rb");
  if (!fp) {
    log_warn("zm_fs_open: 找不到文件 \"%s\" (全路径: %s)", bn, full_path);
    return 0;
  }

  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz < 0) {
    fclose(fp);
    return 0;
  }

  uint8_t *buf = malloc((size_t)sz);
  if (!buf) {
    fclose(fp);
    return 0;
  }
  if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
    free(buf);
    fclose(fp);
    return 0;
  }
  fclose(fp);

  g_file_data = buf;
  g_file_size = (size_t)sz;
  g_file_pos = 0;

  log_info("fs.open(\"%s\") -> FILE1 (size=%zu)", bn, g_file_size);
  return FILE1; // FILE1 是预定义的虚拟句柄地址
}

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

uint32_t zm_file_size(uc_engine *uc, uint32_t file_id) {
  (void)uc;
  if (file_id == FILE1 && g_file_data)
    return (uint32_t)g_file_size;
  return 0;
}

uint32_t zm_fs_release(uc_engine *uc) {
  (void)uc;
  return 0;
}

/* ---------- 默认初始化（仅设置数据目录，不扫描任何文件） ---------- */
void zm_fs_register_default(const char *applet_dir) {
  zm_fs_set_data_dir(applet_dir);
  log_info("zm_fs: 数据目录 = \"%s\"（文件将在 fs.open 时按需加载）",
           s_data_dir);
}

/* ---------- 清理 ---------- */
void zm_fs_shutdown(void) {
  if (g_file_data) {
    free(g_file_data);
    g_file_data = NULL;
  }
  g_file_size = 0;
  g_file_pos = 0;
  s_data_dir[0] = '\0';
}
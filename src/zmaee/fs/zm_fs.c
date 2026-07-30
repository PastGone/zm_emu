#include "zm_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../log/log.h"
#include "../core/zm_addrs.h"
#include "../core/zm_common.h"
#include "../core/zm_str.h" /* read_cstr */

/* ---------- .zmr 资源文件处理 ----------
 *
 * 1. applet 端读取流程（见反编译 sub_1DC）：
 *      fs.open(filename, 1)            -> 返回文件对象（本实现固定 FILE1）
 *      file.read(buf, 4)               -> 读 magic，校验 == 0x30726D7A ("zmr0")
 *      file.read(buf, 4)               -> 读资源个数 N
 *      file.seek(ABS, 4*idx + 8)       -> 定位到偏移表第 idx 项
 *      file.read(buf, 8)               -> 读
 * entry[idx]、entry[idx+1]（起止偏移） file.seek(ABS, entry[idx])      ->
 * 定位到资源数据起始 file.read(buf, entry[idx+1]-entry[idx]) -> 读出资源
 *      file.close()
 *    即 applet 自行解析头部，宿主只需按游标提供原始字节即可。
 *
 * 2. 本模块在载入时额外解析 magic / 资源数 / 偏移表：
 *    - 早期校验 magic，避免误载非 .zmr 文件；
 *    - 暴露 zm_fs_get_resource() 供宿主直接按索引取资源（如音频自测），
 *      不必再走 trap 游标。
 *
 * 文件结构：
 *   0x00  4B   magic (0x30726D7A)
 *   0x04  4B   资源个数 N
 *   0x08  4B*(N+1)  偏移表：entry[i]=第 i 个资源起始绝对偏移，
 *                   entry[N]=数据区末尾（即 CRC 前）
 *   数据区 N 个资源块连续存放
 *   末尾-4 4B  CRC32（原始算法未知，仅记录不参与拆分）
 */

/* ---------- 模块内部状态 ---------- */
static uint8_t *s_zmr_data = NULL; /* 整个 .zmr 文件缓冲 */
static size_t s_zmr_size = 0;      /* 文件总字节数 */
static uint32_t s_file_pos = 0;    /* file.read/file.seek 使用的游标 */

/* 解析出的资源索引（供 zm_fs_get_resource 直接访问） */
static uint32_t s_resource_count = 0;   /* 资源个数 N */
static uint32_t *s_offset_table = NULL; /* 长度 N+1，绝对偏移 */

/* 释放偏移表与缓冲，把状态归零（供 reload / shutdown 复用） */
static void free_state(void) {
  free(s_zmr_data);
  s_zmr_data = NULL;
  s_zmr_size = 0;
  s_file_pos = 0;
  free(s_offset_table);
  s_offset_table = NULL;
  s_resource_count = 0;
}

/* ---------- 多文件层（00000405.app） ----------
 * 句柄 = FILE1 + idx*0x10（idx 0..7）。applet 1 的 .zmr 单游标路径在
 * 句柄未占用时自动回退（file_id==FILE1 且 slot 0 未占用且 .zmr 已载入）。
 */
#define FS_MAX_SLOTS 8
#define FS_SLOT_STRIDE 0x10

typedef struct {
  char name[64];       /* basename，查找键 */
  const uint8_t *data; /* 文件内容（调用方持有，或由 s_owned 持有） */
  size_t size;         /* 文件字节数 */
  uint32_t pos;        /* 当前游标 */
  bool in_use;         /* 是否已打开（fs.open 分配后置 true，close 后 false） */
  bool registered;     /* 是否已登记（区分空槽与已登记未打开） */
} file_slot_t;

static file_slot_t s_slots[FS_MAX_SLOTS];

/* register_default 读入磁盘文件持有的缓冲，shutdown 时统一释放 */
#define FS_MAX_OWNED 8
static uint8_t *s_owned[FS_MAX_OWNED];
static int s_owned_count = 0;

/* 从完整路径取 basename（最后一个 '/' 或 '\\' 之后），写入 out（不超
 * out_cap-1） */
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

bool zm_fs_register_file(const char *name, const void *data, size_t size) {
  if (!name || !data || size == 0)
    return false;
  char bn[64];
  basename_of(name, bn, sizeof(bn));
  for (int i = 0; i < FS_MAX_SLOTS; i++) {
    if (s_slots[i].registered && strcmp(s_slots[i].name, bn) == 0) {
      /* 已登记：更新内容（reload 场景） */
      s_slots[i].data = (const uint8_t *)data;
      s_slots[i].size = size;
      s_slots[i].pos = 0;
      s_slots[i].in_use = false;
      return true;
    }
  }
  for (int i = 0; i < FS_MAX_SLOTS; i++) {
    if (!s_slots[i].registered) {
      strncpy(s_slots[i].name, bn, sizeof(s_slots[i].name) - 1);
      s_slots[i].name[sizeof(s_slots[i].name) - 1] = '\0';
      s_slots[i].data = (const uint8_t *)data;
      s_slots[i].size = size;
      s_slots[i].pos = 0;
      s_slots[i].in_use = false;
      s_slots[i].registered = true;
      return true;
    }
  }
  log_warn("zm_fs_register_file: 句柄表已满，无法登记 %s", bn);
  return false;
}

/* 从磁盘读入一个文件到 malloc 缓冲（由 s_owned 持有），成功返回 true。
 * 缓冲在 zm_fs_shutdown 释放。 */
bool zm_fs_register_hostfile(const char *host_path, const char *reg_name) {
  FILE *f = fopen(host_path, "rb");
  if (!f) {
    log_info("zm_fs: 跳过未找到的文件 %s", host_path);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
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
  if (s_owned_count < FS_MAX_OWNED) {
    s_owned[s_owned_count++] = buf;
  } else {
    /* 超出持有上限：仍登记但无法在 shutdown 释放（不应发生） */
    log_warn("zm_fs: owned 缓冲已满，%s 内存可能泄漏", host_path);
  }
  zm_fs_register_file(reg_name, buf, (size_t)sz);
  log_info("zm_fs: 已登记 %s (size=%ld) <- %s", reg_name, sz, host_path);
  return true;
}

void zm_fs_register_default(const char *applet_dir) {
  /* 推导 applet 目录（去掉末尾的 '/'） */
  char dir[1024];
  strncpy(dir, applet_dir ? applet_dir : ".", sizeof(dir) - 1);
  dir[sizeof(dir) - 1] = '\0';
  size_t dl = strlen(dir);
  while (dl > 0 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\'))
    dir[--dl] = '\0';
  if (dl == 0) {
    strncpy(dir, ".", sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    dl = strlen(dir);
  }

  char path[1280];
  /* app_list 与 res 下的文件；applet 自身 .app 由 main.c 单独登记 */
  snprintf(path, sizeof(path), "%s/app_list/config.b", dir);
  zm_fs_register_hostfile(path, "config.b");
  snprintf(path, sizeof(path), "%s/app_list/zmsys006.dll", dir);
  zm_fs_register_hostfile(path, "zmsys006.dll");
  snprintf(path, sizeof(path), "%s/app_list/zmsys001.dll", dir);
  zm_fs_register_hostfile(path, "zmsys001.dll");
  snprintf(path, sizeof(path), "%s/app_list/1_32icon.zbmp", dir);
  zm_fs_register_hostfile(path, "1_32icon.zbmp");
}

void zm_fs_shutdown(void) {
  free_state();
  for (int i = 0; i < s_owned_count; i++) {
    free(s_owned[i]);
    s_owned[i] = NULL;
  }
  s_owned_count = 0;
  memset(s_slots, 0, sizeof(s_slots));
}

const uint8_t *zm_fs_get_resource(uint32_t index, uint32_t *out_size) {
  if (!s_zmr_data || !s_offset_table || index >= s_resource_count || !out_size)
    return NULL;
  uint32_t start = s_offset_table[index];
  uint32_t end = s_offset_table[index + 1];
  if (start > end || end > (uint32_t)s_zmr_size - 4)
    return NULL;
  *out_size = end - start;
  return s_zmr_data + start;
}

uint32_t zm_fs_get_resource_count(void) { return s_resource_count; }

/* ---------- 多文件 fs.open / read / seek / close ----------
 * 句柄 = FILE1 + idx*0x10。idx 由 (file_id - FILE1)/0x10 计算。
 * 当 file_id==FILE1 且 slot 0 未占用且 .zmr 已载入时，回退旧单游标路径
 * （applet 1 兼容）。
 */

/* 把客户机 filename_ptr 处的文件名解析到 host buf。
 * 兼容两种形态：
 *   (a) zmaee 字符串对象（str_ctor/str_assign 构造：+0=数据指针，+4=长度）
 *   (b) 裸 C 字符串缓冲（sub_84FD8 用 sprintf 拼出的 "%s%08x.app"）
 * 委托 zm_read_str_obj 鲁棒判定，避免把 str_obj 的指针字节误当裸串。
 */
static void read_filename(uc_engine *uc, uint32_t ptr, char *buf, size_t cap) {
  zm_read_str_obj(uc, ptr, buf, cap);
}

/* 句柄 <-> slot 索引转换 */
static int handle_to_idx(uint32_t file_id) {
  if (file_id < FILE1)
    return -1;
  uint32_t off = file_id - FILE1;
  if (off % FS_SLOT_STRIDE != 0)
    return -1;
  int idx = (int)(off / FS_SLOT_STRIDE);
  if (idx < 0 || idx >= FS_MAX_SLOTS)
    return -1;
  return idx;
}

uint32_t zm_fs_open(uc_engine *uc, uint32_t filename_ptr) {
  char name[256];
  read_filename(uc, filename_ptr, name, sizeof(name));
  char bn[64];
  basename_of(name, bn, sizeof(bn));
  log_debug("applet 调用文件打开函数打开文件: %s", bn);

  /* 按 basename 查表 */
  for (int i = 0; i < FS_MAX_SLOTS; i++) {
    if (s_slots[i].registered && strcmp(s_slots[i].name, bn) == 0) {
      s_slots[i].pos = 0;
      s_slots[i].in_use = true;
      uint32_t h = FILE1 + (uint32_t)i * FS_SLOT_STRIDE;
      /* 把 FILE_VT 写入 handle 首字段，使 applet 可通过
       * (*handle)->vt[off] 调用 file 方法（slot0 的 FILE1 已在
       * build_vtables 初始化，其余 slot 需在此补写）。 */
      uc_mem_write(uc, h, &FILE_VT, 4);
      log_info("fs.open(\"%s\") -> handle 0x%X (slot %d, size=%zu)", bn, h, i,
               s_slots[i].size);
      return h;
    }
  }

  /* 未命中：若 .zmr 已载入则回退旧单游标（applet 1） */
  if (s_zmr_data) {
    s_file_pos = 0;
    log_info("fs.open(\"%s\") -> FILE1 (回退 .zmr 单游标)",
             bn[0] ? bn : "<null>");
    return FILE1;
  }

  log_warn("fs.open(\"%s\") 未找到匹配文件 -> 返回 0", bn[0] ? bn : "<null>");
  return 0;
}

uint32_t zm_file_close(uc_engine *uc, uint32_t file_id) {
  (void)uc;
  int idx = handle_to_idx(file_id);
  if (idx >= 0 && s_slots[idx].in_use) {
    s_slots[idx].in_use = false;
    s_slots[idx].pos = 0;
  }
  return 0;
}

uint32_t zm_file_read(uc_engine *uc, uint32_t file_id, uint32_t buf,
                      uint32_t length) {
  int idx = handle_to_idx(file_id);
  if (idx >= 0 && s_slots[idx].in_use) {
    file_slot_t *s = &s_slots[idx];
    uint32_t pos = s->pos;
    uint32_t remain = (s->size >= pos) ? (uint32_t)(s->size - pos) : 0;
    uint32_t n = (length < remain) ? length : remain;
    if (n > 0) {
      uc_mem_write(uc, buf, s->data + pos, n);
      s->pos += n;
    }
    return n;
  }
  /* 回退 .zmr 单游标（applet 1：file_id==FILE1，slot 未占用） */
  if (file_id == FILE1 && s_zmr_data) {
    uint32_t pos = s_file_pos;
    uint32_t remain = (s_zmr_size >= pos) ? (uint32_t)(s_zmr_size - pos) : 0;
    uint32_t n = (length < remain) ? length : remain;
    if (n > 0) {
      uc_mem_write(uc, buf, s_zmr_data + pos, n);
      s_file_pos += n;
    }
    return n;
  }
  return 0;
}

uint32_t zm_file_seek(uc_engine *uc, uint32_t file_id, uint32_t whence,
                      uint32_t offset) {
  (void)uc;
  int idx = handle_to_idx(file_id);
  if (idx >= 0 && s_slots[idx].in_use) {
    file_slot_t *s = &s_slots[idx];
    if (whence == 0)
      s->pos = offset;
    else if (whence == 1)
      s->pos += offset;
    else if (whence == 2)
      s->pos = (uint32_t)s->size + offset;
    return 0;
  }
  /* 回退 .zmr 单游标 */
  if (file_id == FILE1 && s_zmr_data) {
    if (whence == 0)
      s_file_pos = offset;
    else if (whence == 1)
      s_file_pos += offset;
    else if (whence == 2)
      s_file_pos = (uint32_t)s_zmr_size + offset;
  }
  return 0;
}

/* FILE_VT[0x24] file.size(file_id)：返回文件总大小
 * sub_83F90 用 (*FILE_VT[0x24])(handle) 取 config.b 大小并与 0x50 比较。 */
uint32_t zm_file_size(uc_engine *uc, uint32_t file_id) {
  (void)uc;
  int idx = handle_to_idx(file_id);
  if (idx >= 0 && s_slots[idx].in_use)
    return (uint32_t)s_slots[idx].size;
  /* 回退 .zmr 单游标 */
  if (file_id == FILE1 && s_zmr_data)
    return (uint32_t)s_zmr_size;
  return 0;
}

/* FS_VT[+0x04] release：无操作返 0 */
uint32_t zm_fs_release(uc_engine *uc) {
  (void)uc;
  return 0;
}

/* FS_VT[+0x14] chdir(str_obj)：stub，仅日志，返 0。
 * sub_841D4 中 FS.chdir("app_list") 调用。
 * str_obj 为 zmaee 字符串对象（+0=数据指针，+4=长度）。 */
uint32_t zm_fs_chdir(uc_engine *uc, uint32_t str_obj) {
  char dir[128];
  zm_read_str_obj(uc, str_obj, dir, sizeof(dir));
  log_info("fs.chdir(\"%s\") stub", dir[0] ? dir : "<null>");
  return 0;
}

/* FS_VT[+0x30] enumFile(FS, index)：枚举目录下第 index 个文件。
 * sub_82584 调用，期望返回非 0 表找到文件并把文件名写入某个输出。
 * stub 返回 0（无更多文件）使枚举循环立即退出，applet 继续后续流程。
 * 真实实现需遍历 app_list 目录。 */
uint32_t zm_fs_enum(uc_engine *uc, uint32_t fs_obj, uint32_t index) {
  (void)uc;
  (void)fs_obj;
  log_info("fs.enum(%u) -> 0 (no more files)", index);
  return 0;
}

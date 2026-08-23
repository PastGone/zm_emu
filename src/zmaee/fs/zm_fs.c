#include "zm_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../../emu.h"
#include "../../log/log.h"
#include "../../tool/uc_helper.h" /* uc_write32 */
#include "../core/zm_str.h"       /* read_cstr / zm_read_str_obj */

/* ---------- 多句柄虚拟文件系统 ----------
 *
 *  - 最多同时打开 ZM_MAX_FILES 个文件，句柄即 FILE1 区内的对象地址；
 *  - 读取：优先在写沙箱目录查找，其次在数据目录（applet 目录）查找，
 *    再退化到递归搜索数据目录（很多 applet 把资源放在 res/ 子目录里）；
 *  - 写入：全部落在写沙箱目录，绝不改动原始 applet 素材；
 *  - 读文件一次性载入内存，写文件在内存里累积、close 时落盘。
 */

typedef struct {
  bool used;
  bool writable;
  bool dirty;
  uint8_t *data;
  size_t size;
  size_t cap;
  uint32_t pos;
  char name[256];      /* 短文件名 */
  char disk_path[2048] /* 落盘路径（写模式） */;
} ZmFile;

static ZmFile g_files[ZM_MAX_FILES];
static char s_data_dir[1024] = {0};
static char s_write_dir[1024] = {0};
static char s_cwd[256] = {0}; /* applet 的当前工作子目录（相对 data_dir） */
static uint32_t s_open_ok = 0;

/* ---------- .zmr 资源包挂载 ----------
 * ZMAEE 的 .zmr 是自定义资源包：头部 "zmr0" + uint32 条目数 + 条目数×uint32
 * 偏移表，之后紧跟若干裸 GIF（无文件名，按索引访问）。真机引擎打开 .zmr 后
 * 把它当作只读文件系统，fs.enum / fs.open 实际读的是包内条目。这里在注册
 * 数据目录时解析 .zmr，把索引条目暴露成虚拟文件 <base>.zmr<idx>。 */
typedef struct {
  uint8_t *buf;       /* 整个 .zmr 文件内容（常驻） */
  uint32_t *offs;     /* 条目偏移表，长度 count+1（末项 = 文件大小） */
  int count;
  char base[256];     /* 包基名，如 000005f9 */
} ZmZmr;

static ZmZmr g_zmr;

static void zm_zmr_try_load(void) {
  memset(&g_zmr, 0, sizeof(g_zmr));
  if (s_data_dir[0] == '\0')
    return;
  /* base = 数据目录最后一段 */
  const char *base = s_data_dir;
  for (const char *q = s_data_dir; *q; q++)
    if (*q == '/' || *q == '\\')
      base = q + 1;
  snprintf(g_zmr.base, sizeof(g_zmr.base), "%s", base);

  char path[2048];
  snprintf(path, sizeof(path), "%s/%s.zmr", s_data_dir, g_zmr.base);
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return;
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz < 12) {
    fclose(fp);
    return;
  }
  uint8_t *buf = malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
    free(buf);
    fclose(fp);
    return;
  }
  fclose(fp);
  if (memcmp(buf, "zmr0", 4) != 0) {
    free(buf);
    return;
  }
  uint32_t cnt =
      (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) |
      ((uint32_t)buf[7] << 24);
  if (cnt == 0 || cnt > 8192) {
    free(buf);
    return;
  }
  uint32_t *offs = malloc((size_t)(cnt + 1) * sizeof(uint32_t));
  if (!offs) {
    free(buf);
    return;
  }
  for (uint32_t i = 0; i < cnt; i++) {
    offs[i] = (uint32_t)buf[8 + i * 4] | ((uint32_t)buf[9 + i * 4] << 8) |
              ((uint32_t)buf[10 + i * 4] << 16) |
              ((uint32_t)buf[11 + i * 4] << 24);
  }
  offs[cnt] = (uint32_t)sz;
  g_zmr.buf = buf;
  g_zmr.offs = offs;
  g_zmr.count = (int)cnt;
  log_info("zm_fs: 挂载 .zmr 资源包 %s.zmr，共 %d 条 GIF 条目", g_zmr.base,
           cnt);
}

/* 持久内存文件缓存：写模式文件被 close 后保留数据，供后续只读 open 读取。
 * 00000001 等 applet 会先 mode=2 创建存档、close、再 mode=1 回读；若磁盘
 * 上没有写沙箱（GUI 模式无 -o），close 落盘失败，数据必须保留在内存里。 */
#define ZM_MEM_FILES 16
static struct {
  bool used;
  char name[256];
  uint8_t *data;
  size_t size;
} s_mem_files[ZM_MEM_FILES];

static const uint8_t *mem_cache_find(const char *bn, size_t *out_size) {
  for (int i = 0; i < ZM_MEM_FILES; i++) {
    if (s_mem_files[i].used && strcmp(s_mem_files[i].name, bn) == 0) {
      if (out_size)
        *out_size = s_mem_files[i].size;
      return s_mem_files[i].data;
    }
  }
  return NULL;
}

/* 内存缓存中是否存在该名字的文件（即使数据为空）。 */
static bool mem_cache_exists(const char *bn) {
  for (int i = 0; i < ZM_MEM_FILES; i++) {
    if (s_mem_files[i].used && strcmp(s_mem_files[i].name, bn) == 0)
      return true;
  }
  return false;
}

static void mem_cache_put(const char *bn, const uint8_t *data, size_t size) {
  int slot = -1;
  for (int i = 0; i < ZM_MEM_FILES; i++) {
    if (!s_mem_files[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0)
    slot = 0; /* 满则覆盖第一个 */
  if (s_mem_files[slot].data)
    free(s_mem_files[slot].data);
  snprintf(s_mem_files[slot].name, sizeof(s_mem_files[slot].name), "%s", bn);
  if (size > 0 && data) {
    s_mem_files[slot].data = malloc(size);
    if (s_mem_files[slot].data)
      memcpy(s_mem_files[slot].data, data, size);
  } else {
    s_mem_files[slot].data = NULL;
  }
  s_mem_files[slot].size = size;
  s_mem_files[slot].used = true;
}

/* ========== 内部工具 ========== */

static void trim_trailing_sep(char *s) {
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '/' || s[len - 1] == '\\'))
    s[--len] = '\0';
}

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

static bool path_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

static void mkdir_p(const char *path) {
  char tmp[2048];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

/* 在 root 下递归查找文件名为 name 的文件（深度受限），命中写入 out。 */
static bool find_recursive(const char *root, const char *name, int depth,
                           char *out, size_t out_cap) {
  if (depth < 0)
    return false;

  char direct[2048];
  snprintf(direct, sizeof(direct), "%s/%s", root, name);
  if (path_exists(direct)) {
    snprintf(out, out_cap, "%s", direct);
    return true;
  }

  /* 用 opendir 遍历子目录 */
  DIR *d = opendir(root);
  if (!d)
    return false;
  struct dirent *e;
  bool found = false;
  while (!found && (e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.')
      continue;
    char sub[2048];
    snprintf(sub, sizeof(sub), "%s/%s", root, e->d_name);
    struct stat st;
    if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
      found = find_recursive(sub, name, depth - 1, out, out_cap);
    }
  }
  closedir(d);
  return found;
}

/* 解析 applet 给出的名字 → 宿主机读取路径。找不到返回 false。 */
bool zm_fs_resolve_read(const char *name, char *out, size_t out_cap) {
  char bn[256];
  basename_of(name, bn, sizeof(bn));
  if (bn[0] == '\0')
    return false;

  char cand[2048];

  /* 0) 内存缓存（applet 之前写模式创建并关闭过的文件）。
   * 磁盘上可能没有该文件（GUI 模式无写沙箱），但数据仍可用。 */
  if (mem_cache_exists(bn)) {
    snprintf(out, out_cap, "@mem:%s", bn);
    return true;
  }

  /* 1) 写沙箱（applet 之前自己写过的文件） */
  if (s_write_dir[0]) {
    snprintf(cand, sizeof(cand), "%s/%s", s_write_dir, bn);
    if (path_exists(cand)) {
      snprintf(out, out_cap, "%s", cand);
      return true;
    }
  }
  if (s_data_dir[0] == '\0')
    return false;

  /* 2) applet 的当前工作子目录 */
  if (s_cwd[0]) {
    snprintf(cand, sizeof(cand), "%s/%s/%s", s_data_dir, s_cwd, bn);
    if (path_exists(cand)) {
      snprintf(out, out_cap, "%s", cand);
      return true;
    }
  }

  /* 3) 数据目录直接命中 */
  snprintf(cand, sizeof(cand), "%s/%s", s_data_dir, bn);
  if (path_exists(cand)) {
    snprintf(out, out_cap, "%s", cand);
    return true;
  }

  /* 4) applet 原样给的相对路径（可能带子目录） */
  snprintf(cand, sizeof(cand), "%s/%s", s_data_dir, name);
  if (path_exists(cand)) {
    snprintf(out, out_cap, "%s", cand);
    return true;
  }

  /* 5) 递归找两层（res/、sound/、slg/ 之类） */
  return find_recursive(s_data_dir, bn, 2, out, out_cap);
}

static int alloc_slot(void) {
  for (int i = 0; i < ZM_MAX_FILES; i++) {
    if (!g_files[i].used)
      return i;
  }
  return -1;
}

static ZmFile *slot_of(uint32_t handle) {
  if (handle < FILE1 || handle >= FILE1 + ZM_MAX_FILES * ZM_FILE_OBJ_STRIDE)
    return NULL;
  uint32_t off = handle - FILE1;
  if (off % ZM_FILE_OBJ_STRIDE != 0)
    return NULL;
  ZmFile *f = &g_files[off / ZM_FILE_OBJ_STRIDE];
  return f->used ? f : NULL;
}

static void flush_and_close(ZmFile *f) {
  if (!f->used)
    return;
  if (f->writable) {
    /* 写模式文件：先尝试落盘（有写沙箱时），再把数据保留到内存缓存，
     * 供后续只读 open 复用（00000001 的"写→关→读"存档流程）。 */
    if (f->dirty && f->disk_path[0]) {
      FILE *fp = fopen(f->disk_path, "wb");
      if (fp) {
        if (f->size > 0 && f->data)
          fwrite(f->data, 1, f->size, fp);
        fclose(fp);
        log_info("fs: 写回 \"%s\"（%zu 字节）", f->disk_path, f->size);
      } else {
        log_warn("fs: 无法写回 \"%s\": %s", f->disk_path, strerror(errno));
      }
    }
    if (f->name[0])
      mem_cache_put(f->name, f->data, f->size);
  }
  free(f->data);
  memset(f, 0, sizeof(*f));
}

/* ========== 对外 API ========== */

void zm_fs_set_data_dir(const char *dir) {
  if (dir && dir[0]) {
    snprintf(s_data_dir, sizeof(s_data_dir), "%s", dir);
    trim_trailing_sep(s_data_dir);
  } else {
    s_data_dir[0] = '\0';
  }
  s_cwd[0] = '\0';
}

void zm_fs_set_write_dir(const char *dir) {
  if (dir && dir[0]) {
    snprintf(s_write_dir, sizeof(s_write_dir), "%s", dir);
    trim_trailing_sep(s_write_dir);
    mkdir_p(s_write_dir);
    log_info("zm_fs: 写沙箱 = \"%s\"", s_write_dir);
  } else {
    s_write_dir[0] = '\0';
  }
}

/**
 * FileMgr_VT[0x08] open(name, mode)
 * mode 的具体编码因 applet 而异，这里用启发式：
 *   - mode 含 bit1/bit2（2/4）或文件不存在 → 允许写
 *   - 只要能在磁盘上找到就先把内容载入内存
 * 失败返回 0。
 */
uint32_t zm_fileMgr_open_file(uc_engine *uc, uint32_t filename_ptr,
                              uint32_t mode) {
  char name[256];
  zm_read_str_obj(uc, filename_ptr, name, sizeof(name));
  if (name[0] == '\0') {
    log_warn("fs.open: 文件名为空（ptr=0x%08X）", filename_ptr);
    return 0;
  }
  char bn[256];
  basename_of(name, bn, sizeof(bn));

  int idx = alloc_slot();
  if (idx < 0) {
    log_warn("fs.open(\"%s\"): 句柄已用尽", bn);
    return 0;
  }
  ZmFile *f = &g_files[idx];
  memset(f, 0, sizeof(*f));

  /* .zmr 资源包虚拟条目：<base>.zmr<idx> / <base>.zmr.<idx>。
   * 真机引擎把 .zmr 挂成只读文件系统，资源以索引形式存在、无文件名，
   * 这里按约定命名暴露，使加载器能像打开普通文件一样取到对应 GIF。
   * 注意：不能用"纯数字"形式匹配——base 名本身（如 000005f9）以数字开头，
   * sscanf("%d") 会读到前导数字导致整包名被误判成条目。 */
  if (g_zmr.count > 0) {
    int eidx = -1;
    char tmp[256];
    if (sscanf(bn, "%255[^.].zmr%d", tmp, &eidx) == 2 &&
        strcmp(tmp, g_zmr.base) == 0) {
      /* matched <base>.zmr<idx> */
    } else if (sscanf(bn, "%255[^.].zmr.%d", tmp, &eidx) == 2 &&
               strcmp(tmp, g_zmr.base) == 0) {
      /* matched <base>.zmr.<idx> */
    } else {
      eidx = -1;
    }
    if (eidx >= 0 && eidx < g_zmr.count) {
      uint32_t s = g_zmr.offs[eidx];
      uint32_t len = g_zmr.offs[eidx + 1] - s;
      f->data = malloc(len ? len : 1);
      if (f->data && len)
        memcpy(f->data, g_zmr.buf + s, len);
      f->size = len;
      f->cap = len;
      f->pos = 0;
      snprintf(f->name, sizeof(f->name), "%s", bn);
      f->used = true;
      f->writable = false;
      uint32_t handle = ZM_FILE_OBJ(idx);
      uc_write32(uc, handle, FILE_VT);
      s_open_ok++;
      log_info("fs.open(\"%s\", mode=0x%X) -> 0x%08X（%u 字节，.zmr 条目 %d）",
               bn, mode, handle, len, eidx);
      return handle;
    }
  }

  char disk[2048];
  bool found = zm_fs_resolve_read(name, disk, sizeof(disk));
  /* mode 决定读写属性：bit1/bit2（2/4）= 写。只读打开（mode=1，如
   * 0000050b 枚举 "%s\\res\\game%d.ypak"）时文件不存在必须返回 0（失败），
   * 否则 applet 的"读到不存在文件就停止"循环永不退出。
   * 早期实现用 `|| !found` 兜底创建，把只读 open 变成了新建。 */
  bool want_write = (mode & 0x6u) != 0;

  if (found) {
    if (strncmp(disk, "@mem:", 5) == 0) {
      /* 从内存缓存载入（写模式创建后关闭、未落盘的文件） */
      size_t msz = 0;
      const uint8_t *mdata = mem_cache_find(bn, &msz);
      if (mdata && msz > 0) {
        f->data = malloc(msz);
        if (f->data) {
          memcpy(f->data, mdata, msz);
          f->size = msz;
          f->cap = msz;
        }
      }
      log_info("fs.open(\"%s\"): 从内存缓存载入（%zu 字节）", bn, f->size);
    } else {
      FILE *fp = fopen(disk, "rb");
      if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz > 0) {
          f->data = malloc((size_t)sz);
          if (f->data && fread(f->data, 1, (size_t)sz, fp) == (size_t)sz) {
            f->size = (size_t)sz;
            f->cap = (size_t)sz;
          } else {
            free(f->data);
            f->data = NULL;
          }
        }
        fclose(fp);
      }
    }
  }

  /* 先查内存池：如果已有同名文件以写模式打开，直接复用（不查磁盘）。
   * 00000001 等 applet 先 mode=2 创建存档、再 mode=1 回读，两次 open
   * 在同一事件周期内发生，写模式句柄尚未 close 落盘，磁盘上可能为空。 */
  bool found_in_mem = false;
  if (!found && !want_write) {
    for (int i = 0; i < ZM_MAX_FILES; i++) {
      if (g_files[i].used && g_files[i].writable &&
          strcmp(g_files[i].name, bn) == 0) {
        if (g_files[i].size > 0 && g_files[i].data) {
          f->data = malloc(g_files[i].size);
          if (f->data) {
            memcpy(f->data, g_files[i].data, g_files[i].size);
            f->size = g_files[i].size;
            f->cap = g_files[i].size;
          }
        }
        found_in_mem = true;
        found = true; /* 让后续代码把内存数据当"已找到"处理 */
        log_info("fs.open: 内存复用 slot[%d] \"%s\" (%zu 字节)", i, bn, g_files[i].size);
        break;
      }
    }
  }

  if (!found && !want_write) {
    log_warn("fs.open(\"%s\"): 未找到（数据目录 %s）", bn, s_data_dir);
    return 0;
  }

  f->used = true;
  f->writable = want_write;
  f->pos = 0;
  snprintf(f->name, sizeof(f->name), "%s", bn);

  if (want_write) {
    if (s_write_dir[0]) {
      snprintf(f->disk_path, sizeof(f->disk_path), "%s/%s", s_write_dir, bn);
      /* 立即在磁盘上创建文件（即使是空的），保证后续只读 open 能通过
       * path_exists 找到它。否则 applet 的"写→关→读"流程会因磁盘上
       * 不存在而让只读 open 返回失败（sub_16CD4 无法读取刚创建的存档）。 */
      if (!found) {
        FILE *fp = fopen(f->disk_path, "wb");
        if (fp)
          fclose(fp);
      }
    } else if (found) {
      snprintf(f->disk_path, sizeof(f->disk_path), "%s", disk);
    }
  }

  uint32_t handle = ZM_FILE_OBJ(idx);
  /* build_vtables 只把 FILE1 基址的 word0 填成 FILE_VT，
   * 而句柄是 FILE1+i*0x80 的对象数组——第 2 个及以后的对象 word0 全是 0，
   * applet 调 size/seek/read 时 BLX 0 直接跑飞。这里补写 vtable 指针。 */
  uc_write32(uc, handle, FILE_VT);
  s_open_ok++;
  log_info("fs.open(\"%s\", mode=0x%X) -> 0x%08X（%zu 字节%s）", bn, mode,
           handle, f->size, found ? "" : "，新建");
  return handle;
}

uint32_t zm_fileMgr_remove(uc_engine *uc, uint32_t name_ptr) {
  char name[256];
  zm_read_str_obj(uc, name_ptr, name, sizeof(name));
  char bn[256];
  basename_of(name, bn, sizeof(bn));
  if (!s_write_dir[0] || bn[0] == '\0')
    return 0;
  char p[2048];
  snprintf(p, sizeof(p), "%s/%s", s_write_dir, bn);
  int rc = remove(p);
  log_info("fs.remove(\"%s\") -> %d", bn, rc);
  return rc == 0 ? 1 : 0;
}

uint32_t zm_fileMgr_rename(uc_engine *uc, uint32_t old_ptr, uint32_t new_ptr) {
  char a[256], b[256];
  zm_read_str_obj(uc, old_ptr, a, sizeof(a));
  zm_read_str_obj(uc, new_ptr, b, sizeof(b));
  if (!s_write_dir[0])
    return 0;
  char pa[2048], pb[2048];
  char ba[256], bb[256];
  basename_of(a, ba, sizeof(ba));
  basename_of(b, bb, sizeof(bb));
  snprintf(pa, sizeof(pa), "%s/%s", s_write_dir, ba);
  snprintf(pb, sizeof(pb), "%s/%s", s_write_dir, bb);
  int rc = rename(pa, pb);
  log_info("fs.rename(\"%s\" -> \"%s\") -> %d", ba, bb, rc);
  return rc == 0 ? 1 : 0;
}

uint32_t zm_fileMgr_mkdir(uc_engine *uc, uint32_t name_ptr) {
  char name[256];
  zm_read_str_obj(uc, name_ptr, name, sizeof(name));
  if (!s_write_dir[0] || name[0] == '\0')
    return 1;
  char p[2048];
  snprintf(p, sizeof(p), "%s/%s", s_write_dir, name);
  mkdir_p(p);
  log_info("fs.mkdir(\"%s\")", name);
  return 1;
}

uint32_t zm_fileMgr_rmdir(uc_engine *uc, uint32_t name_ptr) {
  char name[256];
  zm_read_str_obj(uc, name_ptr, name, sizeof(name));
  if (!s_write_dir[0] || name[0] == '\0')
    return 0;
  char p[2048];
  snprintf(p, sizeof(p), "%s/%s", s_write_dir, name);
  int rc = rmdir(p);
  log_info("fs.rmdir(\"%s\") -> %d", name, rc);
  return rc == 0 ? 1 : 0;
}

uint32_t zm_fileMgr_exists(uc_engine *uc, uint32_t name_ptr) {
  char name[256];
  zm_read_str_obj(uc, name_ptr, name, sizeof(name));
  char disk[2048];
  bool ok = zm_fs_resolve_read(name, disk, sizeof(disk));
  log_debug("fs.exists(\"%s\") -> %d", name, ok);
  return ok ? 1 : 0;
}

uint32_t zm_fileMgr_stat(uc_engine *uc, uint32_t name_ptr) {
  char name[256];
  zm_read_str_obj(uc, name_ptr, name, sizeof(name));
  if (name[0] == '\0') {
    log_warn("fs.stat: 文件名为空（ptr=0x%08X）", name_ptr);
    return 0;
  }
  char disk[2048];
  if (!zm_fs_resolve_read(name, disk, sizeof(disk))) {
    log_info("fs.stat(\"%s\") -> 0（解析不到）", name);
    return 0;
  }
  struct stat st;
  if (stat(disk, &st) != 0) {
    log_info("fs.stat(\"%s\") -> 0（stat 失败）", name);
    return 0;
  }
  int r = S_ISREG(st.st_mode) ? 1 : 0;
  log_info("fs.stat(\"%s\") -> %d%s", name, r, r ? "" : "（目录/非常规）");
  return r;
}

uint32_t zm_fileMgr_chdir(uc_engine *uc, uint32_t name_ptr) {
  char name[256];
  zm_read_str_obj(uc, name_ptr, name, sizeof(name));
  /* 只记录相对子目录名，不做真正的 chdir，避免影响宿主进程 */
  snprintf(s_cwd, sizeof(s_cwd), "%s", name);
  trim_trailing_sep(s_cwd);
  log_info("fs.chdir(\"%s\")", s_cwd);
  return 1;
}

uint32_t zm_fileMgr_enum(uc_engine *uc, uint32_t dir_ptr, uint32_t out_ptr) {
  char name[256];
  zm_read_str_obj(uc, dir_ptr, name, sizeof(name));
  if (out_ptr && g_zmr.count > 0) {
    uc_write32(uc, out_ptr, (uint32_t)g_zmr.count);
    log_info("fs.enum(\"%s\", out=0x%08X) -> %d 项（.zmr 资源包）", name,
             out_ptr, g_zmr.count);
    return 0;
  }
  log_debug("fs.enum(\"%s\", out=0x%08X) -> 0 项", name, out_ptr);
  if (out_ptr)
    uc_write32(uc, out_ptr, 0);
  return 0;
}

uint32_t zm_file_close(uc_engine *uc, uint32_t file_id) {
  (void)uc;
  ZmFile *f = slot_of(file_id);
  if (!f)
    return 0;
  log_debug("file.close(\"%s\")", f->name);
  flush_and_close(f);
  return 0;
}

uint32_t zm_file_read(uc_engine *uc, uint32_t file_id, uint32_t buf,
                      uint32_t length) {
  ZmFile *f = slot_of(file_id);
  if (!f || !f->data || !buf || !length)
    return 0;

  uint32_t pos = f->pos;
  uint32_t remain = (f->size >= pos) ? (uint32_t)(f->size - pos) : 0;
  uint32_t n = (length < remain) ? length : remain;
  if (n > 0) {
    if (uc_mem_write(uc, buf, f->data + pos, n) != UC_ERR_OK) {
      log_warn("file.read: 写入客户机 0x%08X 失败（%u 字节）", buf, n);
      return 0;
    }
    f->pos += n;
  }
  return n;
}

uint32_t zm_file_write(uc_engine *uc, uint32_t file_id, uint32_t buf,
                       uint32_t length) {
  ZmFile *f = slot_of(file_id);
  if (!f || !buf || !length)
    return 0;
  if (!f->writable) {
    log_debug("file.write(\"%s\"): 只读句柄，忽略 %u 字节", f->name, length);
    return length; /* 假装写成功，避免 applet 反复重试 */
  }

  size_t need = (size_t)f->pos + length;
  if (need > f->cap) {
    size_t ncap = f->cap ? f->cap * 2 : 4096;
    while (ncap < need)
      ncap *= 2;
    uint8_t *nd = realloc(f->data, ncap);
    if (!nd) {
      log_error("file.write: realloc(%zu) 失败", ncap);
      return 0;
    }
    memset(nd + f->cap, 0, ncap - f->cap);
    f->data = nd;
    f->cap = ncap;
  }
  if (uc_mem_read(uc, buf, f->data + f->pos, length) != UC_ERR_OK) {
    log_warn("file.write: 读取客户机 0x%08X 失败（%u 字节）", buf, length);
    return 0;
  }
  f->pos += length;
  if (f->pos > f->size)
    f->size = f->pos;
  f->dirty = true;
  return length;
}

uint32_t zm_file_seek(uc_engine *uc, uint32_t file_id, uint32_t whence,
                      uint32_t offset) {
  (void)uc;
  ZmFile *f = slot_of(file_id);
  if (!f)
    return (uint32_t)-1;

  /* 真机 AEE IFile::seek 的 whence 枚举：0=SET（绝对位置），1=END（相对末尾）。
   * 早期实现误用 0=SET/1=CUR/2=END，导致 seek(1,0) 原地不动、取 size 惯用法失效。 */
  int64_t np = (int64_t)f->pos;
  int32_t off = (int32_t)offset;
  if (whence == 0)
    np = off;
  else if (whence == 1)
    np = (int64_t)f->size + off;

  if (np < 0)
    np = 0;
  if (np > (int64_t)f->size && !f->writable)
    np = (int64_t)f->size;
  f->pos = (uint32_t)np;
  /* 真机 IFile::seek 成功时返回 0（00000102.app 反汇编 CMP R0,#0 确认）。
   * 旧实现返回 f->pos 导致 applet 的 seek→CMP R0,#0 判定失败，音频永不播放。 */
  return 0;
}

uint32_t zm_file_size(uc_engine *uc, uint32_t file_id) {
  (void)uc;
  ZmFile *f = slot_of(file_id);
  return f ? (uint32_t)f->size : 0;
}

uint32_t zm_file_tell(uc_engine *uc, uint32_t file_id) {
  (void)uc;
  ZmFile *f = slot_of(file_id);
  return f ? f->pos : 0;
}

uint32_t zm_fs_release(uc_engine *uc) {
  (void)uc;
  for (int i = 0; i < ZM_MAX_FILES; i++)
    flush_and_close(&g_files[i]);
  return 0;
}

uint32_t zm_fs_open_success_count(void) { return s_open_ok; }

void zm_fs_register_default(const char *applet_dir) {
  zm_fs_set_data_dir(applet_dir);
  zm_zmr_try_load();
  log_info("zm_fs: 数据目录 = \"%s\"（文件将在 fs.open 时按需加载）",
           s_data_dir);
}

void zm_fs_shutdown(void) {
  for (int i = 0; i < ZM_MAX_FILES; i++)
    flush_and_close(&g_files[i]);
  s_data_dir[0] = '\0';
  s_write_dir[0] = '\0';
  s_cwd[0] = '\0';
}

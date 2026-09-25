#include "zm_cbk_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../emu.h"     /* CBK_FILE_VT / CBK_FILE_OBJ（经 emu_mem_regions.h） */
#include "../../log/log.h" /* log_info/log_warn */
#include "../core/zm_str.h" /* read_cstr */
#include "zm_file_mgr.h"    /* zm_fs_read_file（宿主侧整文件读取） */

/* 同时打开的文件数（这一族一次只开一个资源包，4 个足够留余量） */
typedef struct {
  uint8_t *data;  /* 内容（malloc，可能为 NULL = 空文件） */
  uint32_t size;  /* 字节数 */
  uint32_t pos;   /* 读写游标 */
  int used;
  int dirty;      /* 是否被写过（决定关闭时要不要落盘 ✗→✓） */
  char name[128]; /* 打开时的名字（日志用） */
  /* 落盘用的名字：不能用带垃圾前缀的原始串 ✗，要记"实际解析成功的那个"
   * （走 \res\ 兜底时是截断后的名字 ✓；存档这类正常名字就是原样 ✓）。 */
  char wname[256];
} cbk_file_t;

static cbk_file_t s_f[CBK_FILE_MAX];

/* 由包装对象地址反查槽位；不合法/已释放 → NULL */
static cbk_file_t *file_of(uc_engine *uc, uint32_t obj) {
  (void)uc;
  if (obj < CBK_FILE_OBJ ||
      obj >= CBK_FILE_OBJ + CBK_FILE_MAX * CBK_FILE_OBJ_STRIDE)
    return NULL;
  uint32_t i = (obj - CBK_FILE_OBJ) / CBK_FILE_OBJ_STRIDE;
  if (i >= CBK_FILE_MAX || !s_f[i].used)
    return NULL;
  return &s_f[i];
}

uint32_t zm_cbk_file_open(uc_engine *uc, uint32_t path_ptr, uint32_t mode) {
  (void)mode;
  char name[512];
  name[0] = '\0';
  if (path_ptr)
    read_cstr(uc, path_ptr, name, sizeof(name));

  if (!name[0]) {
    log_warn("CBK 文件对象：路径为空（r1=0x%X mode=%u），返回 0", path_ptr,
             (unsigned)mode);
    return 0;
  }

  /* 诊断：把路径的**原始字节**也打出来（这一族的目录名多是 GBK，终端里显示成
   * 乱码 ✗，要按字节判断真名/真目录时才看得清）。ZM_CBK_DBG=1 才打。 */
  if (getenv("ZM_CBK_DBG")) {
    char hex[3 * 64 + 1];
    int q = 0;
    for (unsigned i = 0; name[i] && i < 64; i++)
      q += snprintf(hex + q, sizeof(hex) - (size_t)q, "%02X ", (unsigned char)name[i]);
    log_info("CBK 文件对象：请求路径字节=[%s] (len=%zu)", hex, strlen(name));
  }

  int slot = -1;
  for (uint32_t k = 0; k < CBK_FILE_MAX; k++) {
    if (!s_f[k].used) {
      slot = (int)k;
      break;
    }
  }
  if (slot < 0) {
    log_warn("CBK 文件对象：槽位已满（%u），\"%s\" 打开失败", CBK_FILE_MAX, name);
    return 0;
  }

  uint8_t *buf = NULL;
  size_t len = 0;
  /* 落盘用的名字：默认用原样；若走了下面的 \res\ 兜底，则改用截断后的名字 ✓ */
  char wname[256];
  snprintf(wname, sizeof(wname), "%s", name);
  /* 找不到就按**空文件**处理：applet 拿到的 size=0 → 走"没有内容"分支，
   * 不会再出现"把指针当尺寸去 malloc(16MB)"那种崩法 ✓ */
  if (zm_fs_read_file(name, &buf, &len) != 0) {
    /* ★ 兜底：这一族拼路径用的是 `sprintf("%s%s", 前缀, "\res\...")`，而**前缀
     * 是从我们某个接口拿的、内容是垃圾**（实测 0000050b：请求字节 =
     *   90 08 E6 70 08 E6  +  "\res\game.ypak"
     * ——后一半完全正确 ✗）。与其去修那个前缀的来源（牵连面大），这里只在本族
     * 的"资源工厂"里做一步收敛：从最后一个 "\res\" 截断重试 ✓。
     * 结果就落到 <applet目录>/res/game.ypak —— 仓库里确实有（game.ypak 896KB ✓）。 */
    const char *p = name;
    const char *hit = NULL;
    while ((p = strstr(p, "\\res\\")) != NULL) {
      hit = p;
      p++;
    }
    if (hit && hit != name && zm_fs_read_file(hit, &buf, &len) == 0) {
      log_info("CBK 文件对象：\"%s\" 未找到，按 \".%s\" 重试成功", name, hit);
      snprintf(wname, sizeof(wname), "%s", hit); /* 写回要跟着实际打开的那个 ✓ */
    } else {
      log_info("CBK 文件对象：\"%s\" 不存在 → 空文件（size=0）", name);
      buf = NULL;
      len = 0;
    }
  }

  cbk_file_t *f = &s_f[slot];
  f->data = buf;
  f->size = (uint32_t)len;
  f->pos = 0;
  f->used = 1;
  f->dirty = 0;
  snprintf(f->name, sizeof(f->name), "%s", name);
  snprintf(f->wname, sizeof(f->wname), "%s", wname);

  uint32_t obj = CBK_FILE_OBJ + (uint32_t)slot * CBK_FILE_OBJ_STRIDE;
  uint32_t vt = CBK_FILE_VT;
  uint32_t idx = (uint32_t)slot;
  uc_mem_write(uc, obj, &vt, 4);
  uc_mem_write(uc, obj + 4, &idx, 4);

  log_info("CBK 文件对象：\"%s\" 打开成功（%u 字节），对象=0x%X（槽%d）", name,
           f->size, obj, slot);
  return obj;
}

uint32_t zm_cbk_file_size(uc_engine *uc, uint32_t obj) {
  cbk_file_t *f = file_of(uc, obj);
  if (!f)
    return 0;
  /* ★ 这一族把 +0x24 当"文件大小"用（我们的 IFile 表里同名槽是 Tell ✗）——
   * 这两个对象是**这里**造出来的，所以按它们的语义给"大小" ✓。 */
  return f->size;
}

uint32_t zm_cbk_file_read(uc_engine *uc, uint32_t obj, uint32_t buf,
                          uint32_t len) {
  cbk_file_t *f = file_of(uc, obj);
  if (!f || !buf || !len)
    return 0;
  uint32_t n = (f->pos < f->size) ? (f->size - f->pos) : 0;
  if (n > len)
    n = len;
  if (n && f->data)
    uc_mem_write(uc, buf, f->data + f->pos, n);
  else if (n)
    n = 0; /* 空文件：没有内容可给 */
  f->pos += n;
  return n;
}

uint32_t zm_cbk_file_write(uc_engine *uc, uint32_t obj, uint32_t buf,
                           uint32_t len) {
  cbk_file_t *f = file_of(uc, obj);
  if (!f || !buf || !len)
    return 0;
  /* ★ 改动**先记在内存**，关闭（release）时统一落盘 ✗→✓。
   *
   * 以前这里"只记账、不外写" —— 对**资源包**是对的（读多写少、不该动仓库 ✓），
   * 但对**存档**是致命的 ✗：这一族的"存档/设置"就是"打开（或新建）一个文件 →
   * 写 → 关闭"，只记账就等于**存了个空气** ✓（用户实测："存档功能坏了" ✓）。
   * 现在：只有**真被写过**（dirty ✓）的才落盘，纯读的资源包永远不碰仓库 ✓。 */
  if (f->pos + len > f->size) {
    uint32_t need = f->pos + len;
    if (need > 16u * 1024u * 1024u) {
      log_warn("CBK 文件对象：\"%s\" 写入超 16MB，拒绝", f->name);
      return 0;
    }
    uint8_t *np = (uint8_t *)realloc(f->data, need);
    if (!np)
      return 0;
    if (need > f->size)
      memset(np + f->size, 0, need - f->size);
    f->data = np;
    f->size = need;
  }
  uc_mem_read(uc, buf, f->data + f->pos, len);
  f->pos += len;
  f->dirty = 1; /* 关闭时落盘（见 release） */
  return len;
}

uint32_t zm_cbk_file_seek(uc_engine *uc, uint32_t obj, uint32_t whence,
                          uint32_t off) {
  cbk_file_t *f = file_of(uc, obj);
  if (!f)
    return 0;
  int32_t base = 0;
  if (whence == 1)
    base = (int32_t)f->pos;
  else if (whence == 2)
    base = (int32_t)f->size;
  int32_t p = base + (int32_t)off;
  if (p < 0)
    p = 0;
  if ((uint32_t)p > f->size)
    p = (int32_t)f->size;
  f->pos = (uint32_t)p;
  return f->pos;
}

/* 退出前的兜底：把"写过但一直没 release"的文件也落盘。
 *
 * 为什么需要：不少 applet 存档后**不显式关闭**（写、切场景、直接退），
 * 只在 release 里落盘的话那份存档就丢了 ✗。由 main.c 退出路径调用
 * （zm_fs_shutdown 之后），与 release 共用同一套写回逻辑 ✓。 */
void zm_cbk_file_flush_all(void) {
  for (uint32_t i = 0; i < CBK_FILE_MAX; i++) {
    cbk_file_t *f = &s_f[i];
    if (!f->used || !f->dirty || !f->data || !f->wname[0])
      continue;
    if (getenv("ZM_CBK_NO_WRITEBACK"))
      continue;
    if (zm_fs_write_back_name(f->wname, f->data, f->size) == 0)
      log_info("CBK 文件对象：退出前补写回 \"%s\"（%u 字节）", f->wname, f->size);
    else
      log_warn("CBK 文件对象：退出前写回 \"%s\" 失败 ✗", f->wname);
    f->dirty = 0;
  }
}

uint32_t zm_cbk_file_release(uc_engine *uc, uint32_t obj) {
  cbk_file_t *f = file_of(uc, obj);
  if (!f)
    return 0;
  /* ★ 关闭时落盘：写过（dirty）才写回，纯读的资源包绝不碰仓库 ✓ */
  if (f->dirty && f->data && f->wname[0] && !getenv("ZM_CBK_NO_WRITEBACK")) {
    if (zm_fs_write_back_name(f->wname, f->data, f->size) == 0)
      log_info("CBK 文件对象：\"%s\" 有改动 → **已写回宿主**（%u 字节）",
               f->wname, f->size);
    else
      log_warn("CBK 文件对象：\"%s\" 写回失败（改动只在内存里 ✗）", f->wname);
  } else if (f->dirty) {
    log_info("CBK 文件对象：\"%s\" 有改动但未落盘（ZM_CBK_NO_WRITEBACK）",
             f->wname);
  }
  free(f->data);
  f->data = NULL;
  f->size = 0;
  f->pos = 0;
  f->used = 0;
  log_info("CBK 文件对象：\"%s\" 已释放", f->name);
  f->name[0] = '\0';
  return 0;
}

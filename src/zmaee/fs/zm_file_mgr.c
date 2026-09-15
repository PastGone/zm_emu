#include "zm_file_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* access/F_OK（TestFile 存在性检查） */
#include <iconv.h>  /* GBK→UTF-8（applet 文件名为固件 GBK 编码） */

#include "../../emu.h"
#include "../../log/log.h"
#include "../core/zm_str.h" /* zm_read_str_obj */
#include "zm_file.h"        /* g_file_data / g_file_size / g_file_pos 状态 */

/* ========== 通用单文件系统：G_FileMgr_ADDR 层 ==========
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

/* 文件查找根目录 */
static char s_data_dir[1024] = {0};

/* ========== 内部工具 ========== */
static void read_filename(uc_engine *uc, uint32_t ptr, char *buf, size_t cap) {
  zm_read_str_obj(uc, ptr, buf, cap);
}

/* GBK→UTF-8（applet 的中文文件名为固件 GBK 编码，宿主文件系统是
 * UTF-8；找不到时做一次转换回退）。成功返转换后长度，失败返 -1。 */
static int gbk_to_utf8(const char *in, char *out, size_t out_cap) {
  iconv_t cd = iconv_open("UTF-8", "GBK");
  if (cd == (iconv_t)-1)
    return -1;
  char *src = (char *)in, *dst = out;
  size_t slen = strlen(in), dleft = out_cap - 1;
  size_t r = iconv(cd, &src, &slen, &dst, &dleft);
  iconv_close(cd);
  if (r == (size_t)-1)
    return -1;
  *dst = '\0';
  return (int)(dst - out);
}

/* 判断字符串是否含高位字节（可能是 GBK 中文） */
static int has_high_byte(const char *s) {
  for (; *s; s++)
    if ((unsigned char)*s >= 0x80)
      return 1;
  return 0;
}

/* ========== ConvertFileName 宿主版（RE：ZMAEE_IFileMgr_ConvertFileName）==========
 * 固件流程：归一化（'\'→'/'、解析 ./ 与 ../、压缩分隔符）→ 按盘符分类：
 *   c:/e: → 内置盘（Android 变体映射 /data/data/<pkg>/...，返 1）
 *   其它盘 → 外置卡（Android 变体映射 GetSdcardPath()/mnt/sdcard，返 2；
 *             含 "assets.zip" 返 3 → 走 zip 包测试链
 *             TestPkgItem/TestPkgItemEx + sub_2A75C 缓存句柄）
 * 注意：RE 来自 Android 移植变体——接口同构但存储映射是安卓专属，
 * 模拟器按宿主语义等价实现：所有盘统一映射到 applet 数据目录，
 * 不照搬 /data/data、/mnt/sdcard 等路径。
 * 返回：1=内置盘 2=外置卡 3=assets.zip -1=非法（归一化后为空）。
 * out 收到不含盘符的相对路径（以 '/' 开头）。 */
static int convert_file_name(const char *in, char *out, size_t cap) {
  const char *p = in;
  int cls = 2; /* 无盘符 → 按外置卡语义（数据目录） */
  if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
      p[1] == ':') {
    char d = (char)(p[0] | 0x20);
    cls = (d == 'c' || d == 'e') ? 1 : 2;
    p += 2;
  }
  /* 组件栈解析（RE 同款语义）：记录每段在 out 中的起始偏移，
   * ".." 弹栈、"." 跳过、分隔符压缩 */
  size_t off[64];
  int n = 0;
  size_t o = 0;
  out[0] = '\0';
  while (*p) {
    while (*p == '/' || *p == '\\')
      p++;
    if (!*p)
      break;
    if (p[0] == '.') {
      if (p[1] == '/' || p[1] == '\\' || p[1] == '\0') {
        p++;
        continue;
      }
      if (p[1] == '.' && (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
        p += 2;
        if (n > 0) {
          o = off[--n];
          out[o] = '\0';
        }
        continue;
      }
    }
    if (n >= 64)
      return -1;
    off[n++] = o;
    if (o + 1 < cap)
      out[o++] = '/';
    while (*p && *p != '/' && *p != '\\') {
      if (o + 1 < cap)
        out[o++] = *p;
      p++;
    }
  }
  out[o] = '\0';
  if (o == 0)
    return -1;
  if (strstr(out, "assets.zip"))
    return 3;
  return cls;
}

/* TestFile 专用：按 zmaee 字符串对象布局稳健读取文件名。
 * 固件布局：+0 data_ptr、+12 起内联缓冲（str_assign/str_ctor 同款）。
 * 顺序：解引用 data_ptr → 直接读 ptr+12 → 裸读 ptr。
 * 不复用 zm_read_str_obj 的启发式——内联区首字节为 0 时它会回退到
 * 裸读，把 +0 的指针字节误当文本（00001b62 实测产出"塞"）。 */
static void read_filename_obj(uc_engine *uc, uint32_t ptr, char *buf,
                              size_t cap) {
  buf[0] = '\0';
  if (ptr == 0)
    return;
  uint32_t dp = 0;
  if (uc_mem_read(uc, ptr, &dp, 4) == UC_ERR_OK && dp != 0) {
    read_cstr(uc, dp, buf, (int)cap);
  }
  if (buf[0] == '\0') {
    read_cstr(uc, ptr + 12, buf, (int)cap);
  }
  if (buf[0] == '\0') {
    read_cstr(uc, ptr, buf, (int)cap);
  }
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
/* IFileMgr 通用 stub（g_filemgr_vtbl 未实现槽） */
uint32_t zm_fileMgr_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                         uint32_t r2, uint32_t r3) {
  (void)uc;
  log_debug("fileMgr stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1,
            r2, r3);
  return 0;
}

/* +0x20 TestFile（RE sub_2A7BC）：检查文件是否存在。
 * 固件流程：ConvertFileName 归一化并分类（见 convert_file_name）——
 *   case 0 → 包内资源表（AndroidAEE_TestPkgItem：zip_open+zip_fopen）
 *   case 3 → assets.zip 打包路径（sub_2A75C 缓存句柄 + TestPkgItemEx）
 *   default → 文件系统 access(name, F_OK)
 * 返回：存在 0；不存在 -1；参数空 -4。
 * 模拟器：convert_file_name 统一映射到数据目录；assets.zip 暂不
 * 支持（返 -1，applet 走外置文件回退）。 */
uint32_t zm_fileMgr_TestFile(uc_engine *uc, uint32_t r0, uint32_t name_ptr) {
  if (r0 == 0 || name_ptr == 0)
    return (uint32_t)-4;
  char name[256];
  read_filename_obj(uc, name_ptr, name, sizeof(name));
  /* GBK 中文文件名 → UTF-8 回退（固件 GBK / 宿主 UTF-8） */
  char utf[256];
  if (has_high_byte(name) && gbk_to_utf8(name, utf, sizeof(utf)) > 0)
    snprintf(name, sizeof(name), "%s", utf);
  char rel[512];
  int cls = convert_file_name(name, rel, sizeof(rel));
  if (cls < 0) {
    log_info("fileMgr.TestFile(\"%s\") -> -1 (归一化后为空)", name);
    return (uint32_t)-1;
  }
  if (cls == 3) {
    /* assets.zip 打包路径（RE：TestPkgItemEx 走 zip_fopen 成员测试）——
     * 模拟器暂不支持 zip 资源，返不存在，applet 走外置文件回退 */
    log_info("fileMgr.TestFile(\"%s\") -> -1 (assets.zip 暂不支持)", name);
    return (uint32_t)-1;
  }
  int ok = 0;
  char full[1280];
  if (s_data_dir[0]) {
    snprintf(full, sizeof(full), "%s%s", s_data_dir, rel);
    if (access(full, F_OK) == 0)
      ok = 1;
    if (!ok) { /* 回退 app_list/ 子目录（与 open_file 一致） */
      snprintf(full, sizeof(full), "%s/app_list%s", s_data_dir, rel);
      if (access(full, F_OK) == 0)
        ok = 1;
    }
  }
  if (!ok && access(rel + 1, F_OK) == 0) /* 相对 CWD（rel 以 '/' 开头） */
    ok = 1;
  log_info("fileMgr.TestFile(\"%s\" cls=%d) -> %d", name, cls, ok ? 0 : -1);
  return ok ? 0 : (uint32_t)-1;
}

/* +0x0C = IFileMgr::GetInfo（RE sub_2A550，参考 ZMAEE 实现）：
 *   int GetInfo(mgr, const char *name, info_out *out)
 *   参数空 → -4；整个 out 结构先 memset 成 0（0x4C 字节）；
 *   目录 → out[0]=2，普通文件 → out[0]=0；
 *   out+4 = 大小(高32)   out+8 = 大小(低32)   out+12 = 文件名（basename）
 *   文件不存在 → -1；成功 → 0
 *
 * 为什么必须实现：00000001 装载 res\sprite\*.zmspx 时先调本槽拿长度，
 * 再用它当 IFile::Read 的长度。以前这里是返回 0 的桩 —— 长度被填 0，
 * Read 读 0 字节（真机 ZMAEE_IFile_Read 对 n=0 同样是返回 0、不拷贝），
 * 于是资源内容一个字节都没进缓冲，后续 sub_15684 解析 zms2 头失败返回 0，
 * 列表项的自表指针变成 0，最终在 sub_15964 里读 [0+0x18] 越界
 * → 崩溃 err=6 pc=0x15994。 */
uint32_t zm_fileMgr_GetInfo(uc_engine *uc, uint32_t r0, uint32_t name_ptr,
                            uint32_t out_ptr) {
  if (r0 == 0 || name_ptr == 0 || out_ptr == 0)
    return (uint32_t)-4;

  /* 与真机一致：先把整个 0x4C 结构清零 */
  {
    static const uint8_t z0[0x4C] = {0};
    uc_mem_write(uc, out_ptr, z0, sizeof(z0));
  }

  char name[256];
  /* ★ 必须用 read_filename（字串），不是 read_filename_obj（带对象头的那种）
   * —— 这里 applet 传的是 sprintf 出来的纯 C 串，用 obj 版会吃掉首字符
   * （实测 "xiao_hua_normal.zmspx" 变成 "iao_hua_normal.zmspx" → 找不到
   * → 大小 0 → Read 读 0 字节 → 资源没装载 → 崩）。 */
  read_filename(uc, name_ptr, name, sizeof(name));
  char utf[256];
  if (has_high_byte(name) && gbk_to_utf8(name, utf, sizeof(utf)) > 0)
    snprintf(name, sizeof(name), "%s", utf);

  /* 取大小：走与 open/read_file 同一套路径解析（数据目录 / app_list 回退） */
  uint8_t *buf = NULL;
  size_t len = 0;
  if (zm_fs_read_file(name, &buf, &len) != 0 || !buf) {
    log_info("fileMgr.GetInfo(\"%s\") -> -1 (找不到, out=0x%X)", name, out_ptr);
    return (uint32_t)-1; /* 不存在 */
  }
  free(buf);

  uint32_t cls = 0; /* 0 = 普通文件 */
  uc_mem_write(uc, out_ptr + 0, &cls, 4);
  uint32_t hi = 0, lo = (uint32_t)len;
  uc_mem_write(uc, out_ptr + 4, &hi, 4);
  uc_mem_write(uc, out_ptr + 8, &lo, 4);

  /* basename（真机 strcpy(a3+12, 最后一个 '/' 之后)） */
  const char *base = name;
  for (const char *p = name; *p; p++)
    if (*p == '/' || *p == '\\')
      base = p + 1;
  uc_mem_write(uc, out_ptr + 12, base, strlen(base) + 1);

  log_info("fileMgr.GetInfo(\"%s\") -> 0 (大小=%u, out=0x%X)", name,
           (unsigned)len, out_ptr);
  return 0;
}

/* +0x30 存储区支持查询（RE sub_29E40）。
 * 返回 ASCII 盘符代码：67='C'（内置盘）、69='E'、84='T'（SD）。
 * a2>=2 时固件走完整 JNI 链（RE：AndroidAEE_CallIntMethod →
 * AEEJni_GetEnv → FindClass("com/zmapp/aee/AEEJNIBridge") →
 * GetMethodID("isSDCardMounted") → CallIntMethod）向 Java 宿主查询
 * SD 卡挂载状态。模拟器角色即"宿主替身"（同 GetTickCount→SDL_GetTicks
 * 的边界决策），宿主目录即"卡"，恒视为已挂载返 84。 */
uint32_t zm_fileMgr_StorageSupport(uc_engine *uc, uint32_t r0, uint32_t type) {
  if (r0 == 0)
    return 0;
  if (type == 0)
    return 67; /* 'C' 内置盘 */
  if (type == 1)
    return 69; /* 'E' */
  return 84;   /* 'T' SD 卡（恒挂载） */
}

uint32_t zm_fileMgr_open_file(uc_engine *uc, uint32_t filename_ptr) {
  (void)uc;
  char name[256];
  read_filename(uc, filename_ptr, name, sizeof(name));
  char rel[512];
  int cls = convert_file_name(name, rel, sizeof(rel));
  if (cls < 0) {
    log_warn("fs.open(\"%s\") -> 0 (归一化后为空)", name);
    return 0;
  }

  /* 先关闭之前打开的文件 */
  if (g_file_data) {
    free(g_file_data);
    g_file_data = NULL;
    g_file_size = 0;
    g_file_pos = 0;
  }

  if (s_data_dir[0] == '\0') {
    log_error("zm_fs_open: 未设置数据目录，无法打开 \"%s\"", name);
    return 0;
  }

  char full_path[1280];
  snprintf(full_path, sizeof(full_path), "%s%s", s_data_dir, rel);

  FILE *fp = fopen(full_path, "rb");
  /* 回退：00000405 的 \config.b 实际在 app_list/ 子目录下
   * （applet 运行工作目录是 app_list）。 */
  if (!fp) {
    char alt[1536];
    snprintf(alt, sizeof(alt), "%s/app_list%s", s_data_dir, rel);
    fp = fopen(alt, "rb");
    if (fp)
      snprintf(full_path, sizeof(full_path), "%s", alt);
  }
  if (!fp) {
    log_warn("zm_fs_open: 找不到文件 \"%s\" (全路径: %s)", name, full_path);
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
  g_file_dirty = 0;
  /* 记住宿主机全路径：Write 之后 close 时要靠它写回（自动存档） */
  snprintf(g_file_path, sizeof(g_file_path), "%s", full_path);

  log_info("fs.open(\"%s\") -> FILE1 (size=%zu)", name, g_file_size);
  return FILE1; /* FILE1 是预定义的虚拟句柄地址 */
}

uint32_t zm_fs_release(uc_engine *uc) {
  (void)uc;
  return 0;
}

/* 宿主侧整文件读取：与 open_file 同一套路径解析（数据目录 / app_list 回退），
 * 但不占用 FILE1 单例句柄——IImage 这类"内部再读一个文件"的场景必须用它，
 * 否则会把 applet 正在解析的文件句柄顶掉。 */
int zm_fs_read_file(const char *name, uint8_t **out_buf, size_t *out_len) {
  if (out_buf)
    *out_buf = NULL;
  if (out_len)
    *out_len = 0;
  if (!name || !name[0] || !out_buf || !out_len)
    return -1;

  char utf[256];
  if (has_high_byte(name) && gbk_to_utf8(name, utf, sizeof(utf)) > 0)
    name = utf;

  char rel[512];
  if (convert_file_name(name, rel, sizeof(rel)) < 0)
    return -1;

  FILE *fp = NULL;
  char full[1280];
  if (s_data_dir[0]) {
    snprintf(full, sizeof(full), "%s%s", s_data_dir, rel);
    fp = fopen(full, "rb");
    if (!fp) {
      snprintf(full, sizeof(full), "%s/app_list%s", s_data_dir, rel);
      fp = fopen(full, "rb");
    }
  }
  if (!fp) {
    fp = fopen(rel + 1, "rb"); /* 相对 CWD（rel 以 '/' 开头） */
    if (fp)
      snprintf(full, sizeof(full), "%s", rel + 1);
  }
  if (!fp) {
    log_warn("zm_fs_read_file: 找不到文件 \"%s\"", name);
    return -1;
  }

  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz < 0) {
    fclose(fp);
    return -1;
  }
  uint8_t *buf = malloc((size_t)sz ? (size_t)sz : 1);
  if (!buf) {
    fclose(fp);
    return -1;
  }
  if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
    free(buf);
    fclose(fp);
    return -1;
  }
  fclose(fp);
  *out_buf = buf;
  *out_len = (size_t)sz;
  log_debug("zm_fs_read_file(\"%s\") -> %zu 字节", name, (size_t)sz);
  return 0;
}

/* ---------- 默认初始化（仅设置数据目录，不扫描任何文件） ---------- */
void zm_fs_register_default(const char *applet_dir) {
  zm_fs_set_data_dir(applet_dir);
  log_info("zm_fs: 数据目录 = \"%s\"（文件将在 fs.open 时按需加载）",
           s_data_dir);
}

/* ---------- 写回宿主机文件 ----------
 * IFile.Write 改动的内容最终由这里落盘。
 * 00000506 每 10 秒写一次 40 字节的存档 `data`（SetTimer(10000, cb=0x9F84)）；
 * 以前 +0x0C 这个槽没接，applet 的存档/设置从来没能保存。 */
int zm_fs_write_back(const char *full_path, const uint8_t *data, size_t len) {
  if (!full_path || !full_path[0])
    return -1;
  FILE *fp = fopen(full_path, "wb");
  if (!fp) {
    log_error("zm_fs_write_back: 打不开 \"%s\" 写入", full_path);
    return -1;
  }
  if (len && fwrite(data, 1, len, fp) != len) {
    fclose(fp);
    log_error("zm_fs_write_back: 写 \"%s\" 失败", full_path);
    return -1;
  }
  fclose(fp);
  return 0;
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

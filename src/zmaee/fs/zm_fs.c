#include "zm_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../log/log.h"
#include "../core/zm_addrs.h"
#include "../core/zm_common.h"

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

/* 标准 CRC32（IEEE 802.3，与 zlib.crc32 一致），仅用于日志比对 */
static uint32_t crc32_compute(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
  }
  return ~crc;
}

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

bool zm_fs_load_zmr(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    log_error("zm_fs_load_zmr: 无法打开 %s", path);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    return false;
  }
  /* 最小长度：magic(4) + count(4) + offset_table(至少 8) + crc(4) */
  if (sz < 20) {
    log_error("zm_fs_load_zmr: 文件过小 (%ld 字节)", sz);
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

  /* 解析 magic */
  uint32_t magic;
  memcpy(&magic, buf, 4);
  if (magic != 0x30726D7Au) { /* "zmr0" */
    log_error("zm_fs_load_zmr: magic 错误 0x%08X（期望 0x30726D7A）", magic);
    free(buf);
    return false;
  }

  /* 解析资源个数 N */
  uint32_t count;
  memcpy(&count, buf + 4, 4);

  /* 解析偏移表：N+1 项，每项 4 字节，起始于 0x08 */
  size_t table_off = 8;
  size_t table_bytes = (size_t)(count + 1) * 4;
  size_t data_start = table_off + table_bytes; /* entry[0] 应等于此值 */
  if (data_start > (size_t)sz - 4) {           /* 至少要留 4 字节 CRC */
    log_error("zm_fs_load_zmr: 偏移表越界 (N=%u)", count);
    free(buf);
    return false;
  }
  uint32_t *table = malloc(table_bytes);
  if (!table) {
    free(buf);
    return false;
  }
  memcpy(table, buf + table_off, table_bytes);

  /* 边界校验：entry[0]==数据区起点，entry[N]==数据区末尾（CRC 前） */
  uint32_t data_end = (uint32_t)((size_t)sz - 4);
  if (table[0] != data_start) {
    log_warn("zm_fs_load_zmr: entry[0]=%u 与数据区起点=%zu 不一致", table[0],
             data_start);
  }
  if (count > 0 && table[count] != data_end) {
    log_warn("zm_fs_load_zmr: entry[N]=%u 与数据区末尾=%u 不一致", table[count],
             data_end);
  }

  /* CRC 仅记录比对（原始算法未知，不阻断载入） */
  uint32_t stored_crc;
  memcpy(&stored_crc, buf + (size_t)sz - 4, 4);
  uint32_t calc_crc = crc32_compute(buf, (size_t)sz - 4);
  log_info(
      ".zmr 载入: %s  size=%zu  N=%u  CRC stored=0x%08X calc=0x%08X（仅记录）",
      path, (size_t)sz, count, stored_crc, calc_crc);

  /* 提交：释放旧状态，替换为新解析结果 */
  free_state();
  s_zmr_data = buf;
  s_zmr_size = (size_t)sz;
  s_file_pos = 0;
  s_resource_count = count;
  s_offset_table = table;
  return true;
}

void zm_fs_shutdown(void) { free_state(); }

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

/* fs.open：打开文件，返回文件对象地址（固定 FILE1），并重置游标。
 * 调用约定（见 sub_1DC）：r0=this(FS), r1=filename, r2=mode。
 * 本实现只有一个 .zmr，故忽略文件名匹配，仅记录便于调试。 */
uint32_t zm_fs_open(uc_engine *uc, uint32_t filename_ptr) {
  char name[64];
  name[0] = '\0';
  if (filename_ptr) {
    /* 安全读取文件名：最多 sizeof-1 字节，遇 NUL 截断 */
    uint8_t tmp[sizeof(name)];
    size_t n = sizeof(tmp) - 1;
    /* 逐段读取，遇 NUL 即止 */
    bool ok = true;
    for (size_t i = 0; i < n; i++) {
      uint8_t ch;
      if (uc_mem_read(uc, filename_ptr + (uint32_t)i, &ch, 1) != UC_ERR_OK) {
        ok = false;
        break;
      }
      tmp[i] = ch;
      if (ch == 0) {
        n = i;
        break;
      }
      n = i + 1;
    }
    if (ok) {
      memcpy(name, tmp, n);
      name[n] = '\0';
    }
  }
  s_file_pos = 0;
  log_info("fs.open(%s) -> FILE1, 游标重置", name[0] ? name : "<null>");
  return FILE1;
}

/* file.close：固定返回 0 */
uint32_t zm_file_close(uc_engine *uc) {
  (void)uc;
  return 0;
}

/* file.read：从当前游标读取 length 字节到客户机 buf。
 * 调用约定：r0=this(file), r1=buf, r2=length。返回实际读取字节数。 */
uint32_t zm_file_read(uc_engine *uc, uint32_t buf, uint32_t length) {
  if (!s_zmr_data)
    return 0;
  uint32_t pos = s_file_pos;
  /* 剩余可读字节数（防止 size - pos 在 pos 越界时下溢） */
  uint32_t remain = (s_zmr_size >= pos) ? (uint32_t)(s_zmr_size - pos) : 0;
  uint32_t n = (length < remain) ? length : remain;
  if (n > 0) {
    uc_mem_write(uc, buf, s_zmr_data + pos, n);
    s_file_pos += n;
  }
  return n;
}

/* file.seek：移动文件游标。
 * 调用约定：r0=this(file), r1=whence, r2=offset。
 * whence: 0=绝对(SEEK_SET), 1=相对当前(SEEK_CUR), 2=相对末尾(SEEK_END)。
 * 返回固定 0。 */
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

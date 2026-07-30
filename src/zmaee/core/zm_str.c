#include "zm_str.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../log/log.h"
#include "zm_addrs.h"

/* 读取客户机地址 addr 处的 C 字符串到宿主机 buf，最多 maxlen-1 字符 */
char *read_cstr(uc_engine *uc, uint32_t addr, char *buf, size_t maxlen) {
  if (addr == 0 || maxlen == 0) {
    if (maxlen > 0)
      buf[0] = '\0';
    return buf;
  }
  size_t i = 0;
  while (i < maxlen - 1) {
    uint8_t c;
    uc_mem_read(uc, addr + i, &c, 1);
    if (c == 0)
      break;
    buf[i++] = c;
  }
  buf[i] = '\0';
  return buf;
}

/**
 * @brief 安全拷贝内存，取源长度和目标容量的较小值，防止越界
 * @param src     源地址 (对应 r0)
 * @param src_len 源缓冲区最大长度 (对应 r1)
 * @param dst     目标地址 (对应 r2)
 * @param dst_len 目标缓冲区容量 (对应 r3)
 * @return 实际拷贝的字节数 (n)
 */
uint32_t zm_strcpy(uc_engine *uc, uint32_t src, uint32_t src_len, uint32_t dst,
                   uint32_t dst_len) {
  // 取较小值作为拷贝长度（对应 r1 < r3 ? r1 : r3）
  size_t n = (src_len < dst_len) ? src_len : dst_len;
  uint8_t *data = malloc(n);

  if (data) {
    uc_mem_read(uc, src, data, n);
    uc_mem_write(uc, dst, data, n);
    free(data);
  }

  return n;
}

/**
 * @brief 格式化字符串并写入客户机目标地址（模拟 sprintf，多参数）
 * @param dest_addr 目标缓冲区客户机地址 (对应 r0)
 * @param fmt_addr  格式字符串客户机地址 (对应 r1)
 * @param args_addr 参数列表基址 (对应 r2)，第一个参数位于 args_addr + 4
 * @return 写入目标缓冲区的字符串长度（不含结尾 '\0'）
 *
 * @note 遍历 fmt，遇 % 解析 flags/width/precision/length/conversion，
 *       依次从 args_addr + 4 起按 4B 取参数（%f 取 8B）。
 *       支持 d/i/u/x/X/o/c/s/p/f/g/e 等，足够
 *       "%s%08x.app" 与 "&dllversion=%d&dllname=%s" 等调用点。
 */
uint32_t zm_sprintf(uc_engine *uc, uint32_t dest_addr, uint32_t fmt_addr,
                    uint32_t args_addr) {
  char fmt[256];
  read_cstr(uc, fmt_addr, fmt, sizeof(fmt));
  log_debug("格式化字符串为: %s", fmt);
  char out[512];
  size_t oi = 0;
  uint32_t arg_off = 4; /* 第一个参数位于 args_addr + 4 */
  size_t flen = strlen(fmt);
  size_t out_cap = sizeof(out);

  for (size_t fi = 0; fi < flen && oi < out_cap - 1;) {
    if (fmt[fi] != '%') {
      out[oi++] = fmt[fi++];
      continue;
    }
    /* 收集完整转换说明（'%' 起到 conversion char） */
    char spec[32];
    size_t si = 0;
    spec[si++] = '%';
    fi++;
    /* flags */
    while (fi < flen && strchr("-+ #0", fmt[fi]) && si < sizeof(spec) - 2)
      spec[si++] = fmt[fi++];
    /* width */
    while (fi < flen && (isdigit((unsigned char)fmt[fi]) || fmt[fi] == '*') &&
           si < sizeof(spec) - 2)
      spec[si++] = fmt[fi++];
    /* precision */
    if (fi < flen && fmt[fi] == '.') {
      spec[si++] = fmt[fi++];
      while (fi < flen && (isdigit((unsigned char)fmt[fi]) || fmt[fi] == '*') &&
             si < sizeof(spec) - 2)
        spec[si++] = fmt[fi++];
    }
    /* length modifiers */
    while (fi < flen && strchr("lhLjz", fmt[fi]) && si < sizeof(spec) - 2)
      spec[si++] = fmt[fi++];
    if (fi >= flen)
      break;
    char conv = fmt[fi++];
    spec[si++] = conv;
    spec[si] = '\0';

    int written = 0;
    switch (conv) {
    case 'd':
    case 'i': {
      uint32_t v = 0;
      uc_mem_read(uc, args_addr + arg_off, &v, 4);
      arg_off += 4;
      written = snprintf(out + oi, out_cap - oi, spec, (int32_t)v);
      break;
    }
    case 'u':
    case 'x':
    case 'X':
    case 'o':
    case 'p': {
      uint32_t v = 0;
      uc_mem_read(uc, args_addr + arg_off, &v, 4);
      arg_off += 4;
      written = snprintf(out + oi, out_cap - oi, spec, v);
      break;
    }
    case 'c': {
      uint32_t v = 0;
      uc_mem_read(uc, args_addr + arg_off, &v, 4);
      arg_off += 4;
      written = snprintf(out + oi, out_cap - oi, spec, (int)v);
      break;
    }
    case 's': {
      uint32_t v = 0;
      uc_mem_read(uc, args_addr + arg_off, &v, 4);
      arg_off += 4;
      char s[256];
      read_cstr(uc, v, s, sizeof(s));
      written = snprintf(out + oi, out_cap - oi, spec, s);
      break;
    }
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'e':
    case 'E': {
      /* double 8B，8 字节对齐 */
      if (arg_off & 4)
        arg_off += 4;
      uint64_t v = 0;
      uc_mem_read(uc, args_addr + arg_off, &v, 8);
      arg_off += 8;
      double d;
      memcpy(&d, &v, 8);
      written = snprintf(out + oi, out_cap - oi, spec, d);
      break;
    }
    case '%':
      out[oi++] = '%';
      written = 0;
      break;
    default:
      /* 未知转换：原样输出 % 与字符 */
      out[oi++] = '%';
      if (oi < out_cap - 1)
        out[oi++] = conv;
      written = 0;
      break;
    }
    if (written > 0)
      oi += (size_t)written;
    else if (written < 0)
      break; /* snprintf 出错 */
  }
  out[oi] = '\0';
  log_debug("最终的拼接结果为: %s", out);

  uc_mem_write(uc, dest_addr, out, oi + 1);
  return (uint32_t)oi;
}

/**
 * @brief 将客户机字符串封装为一个自描述结构体（指针+长度），并写回客户机
 *
 * 结构体布局（对应 main.txt.c 中 root.str_ctor 的实现）：
 *   +0  : 数据指针（指向 +12 处的内联缓冲区）
 *   +4  : 字符串长度
 *   +8  : 容量（与长度相同）
 *   +12 : 内联字符串数据（含 '\0'）
 *
 * @param dest_struct 结构体基址（对应 r0）
 * @param src_str     源字符串地址（对应 r1）
 * @return 返回 dest_struct（即 r0）
 */
uint32_t zm_str_ctor(uc_engine *uc, uint32_t dest_struct, uint32_t src_str) {
  if (dest_struct == 0 || src_str == 0)
    return dest_struct;

  char cstr[256];
  read_cstr(uc, src_str, cstr, sizeof(cstr));
  log_debug("str_ctor 复制: '%s' -> 0x%08X", cstr, dest_struct);

  uint32_t clen = (uint32_t)strlen(cstr);
  if (clen > 255)
    clen = 255;

  /*
   * 00000102.app 中的实际用法是把扩展名直接写到已有字符串缓冲区里：
   *   str_ctor(instance+0x214, "00000102.app")   // 复制完整文件名
   *   str_find(..., '.')                         // 找到 '.'
   *   str_ctor('.'+1, "zmr")                     // 把 "app" 替换成 "zmr"
   * 因此这里按 C 字符串拷贝处理，而不是构造带 header 的字符串对象。
   */
  uc_mem_write(uc, dest_struct, cstr, clen + 1);

  return dest_struct;
}

/**
 * @brief root.spec_lookup：按单字符查规格
 *
 * 读取 ch_addr 处的一个字符，若属于 "opusxXcdf" 之一，则把它写到
 * DUMMY_BUF 并返回 DUMMY_BUF；否则返回 0 表示未找到。
 *
 * @param ch_addr 存放待查字符的客户机地址（对应 r0）
 * @return 命中返回 DUMMY_BUF，未命中返回 0
 */
uint32_t zm_spec_lookup(uc_engine *uc, uint32_t ch_addr) {
  char ch[2] = {0, 0};
  read_cstr(uc, ch_addr, ch, sizeof(ch));
  if (ch[0] && strchr("opusxXcdf", ch[0])) {
    uint8_t val = (uint8_t)ch[0];
    uc_mem_write(uc, DUMMY_BUF, &val, 1);
    return DUMMY_BUF;
  }
  return 0;
}

/**
 * @brief root.str_find：在字符串对象中查找字符
 *
 * str_obj_ptr 指向一个字符串对象，其首字段（+0）为 C 字符串指针。
 * 在该字符串中查找字符 ch（取低 8 位），命中返回 字符串基址+偏移，
 * 未命中返回 0。
 *
 * @param str_obj_ptr 字符串对象基址（对应 r0）
 * @param ch          待查找字符（对应 r1，取低 8 位）
 * @return 命中返回子指针，未命中返回 0
 */
uint32_t zm_str_find(uc_engine *uc, uint32_t str_obj_ptr, uint32_t ch) {
  if (str_obj_ptr == 0)
    return 0;

  /*
   * 兼容两种形态：
   *   (a) zmaee 字符串对象：+0=data_ptr，+4=len，data_ptr 可映射。
   *   (b) 裸 C 字符串缓冲区：str_ctor 把文件名直接复制到 instance+0x214
   *       后，str_find 需要能直接在缓冲区里查找字符。
   */
  uint32_t data_ptr = 0, len = 0;
  bool as_obj = false;
  if (uc_mem_read(uc, str_obj_ptr, &data_ptr, 4) == UC_ERR_OK &&
      uc_mem_read(uc, str_obj_ptr + 4, &len, 4) == UC_ERR_OK) {
    if (data_ptr == str_obj_ptr + 12) {
      as_obj = true; /* str_assign/str_ctor 的内联对象布局 */
    } else if (data_ptr != 0 && len < 4096) {
      uint8_t first = 0;
      if (uc_mem_read(uc, data_ptr, &first, 1) == UC_ERR_OK &&
          (first == 0 || (first >= 0x20 && first < 0x80))) {
        as_obj = true;
      }
    }
  }

  uint32_t base = as_obj ? data_ptr : str_obj_ptr;
  char cstr[256];
  read_cstr(uc, base, cstr, sizeof(cstr));

  char needle = (char)(ch & 0xFF);
  char *p = strchr(cstr, needle);
  if (p)
    return base + (uint32_t)(p - cstr);
  return 0;
}

/**
 * @brief 鲁棒读取"可能是 zmaee 字符串对象"的客户机地址
 *
 * 详见 zm_str.h 注释。判定优先级：
 *   1. data_ptr==ptr+12（str_ctor/str_assign 内联布局）→ 解引用
 *   2. data_ptr 可读且首字节可打印/0 且 len<4096 → 解引用
 *   3. 否则按裸 C 串读取 ptr
 *
 * 不依赖内存布局常量，仅靠 uc_mem_read 返回值判定可读性，
 * 避免把指向未映射区间的"指针"误当 str_obj 解引用。
 */
uint32_t zm_read_str_obj(uc_engine *uc, uint32_t ptr, char *buf, size_t cap) {
  if (buf && cap)
    buf[0] = '\0';
  if (ptr == 0 || cap == 0)
    return 0;

  uint32_t data_ptr = 0, len = 0;
  bool as_obj = false;
  if (uc_mem_read(uc, ptr, &data_ptr, 4) == UC_ERR_OK &&
      uc_mem_read(uc, ptr + 4, &len, 4) == UC_ERR_OK) {
    if (data_ptr == ptr + 12) {
      /* str_ctor/str_assign 的内联布局 */
      as_obj = true;
    } else if (data_ptr != 0 && len < 4096) {
      /* 可能为堆/栈字符串对象：验证 data_ptr 处首字节可读且合理 */
      uint8_t first = 0;
      if (uc_mem_read(uc, data_ptr, &first, 1) == UC_ERR_OK &&
          (first == 0 || (first >= 0x20 && first < 0x80))) {
        as_obj = true;
      }
    }
  }

  if (as_obj) {
    read_cstr(uc, data_ptr, buf, cap);
    /* 若解引用得到空串，但裸串形态可能有效，则回退尝试裸串 */
    if (buf[0] != '\0')
      return (uint32_t)strlen(buf);
  }

  /* 裸 C 字符串形态（sprintf 拼出的路径等） */
  read_cstr(uc, ptr, buf, cap);
  return (uint32_t)strlen(buf);
}

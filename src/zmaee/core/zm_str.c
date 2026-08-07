#include "zm_str.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../emu.h"
#include "../../log/log.h"

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
 * @brief ROOT[0x20] str_copy：把 src 的 UTF-8 字节流转换为 UTF-16 写入 dst。
 *
 * ZMAEE 字符串对象内部是 UTF-16（applet 的 sub_2F060 分配 2+2*len 字节的
 * UTF-16 缓冲后调本函数）。若只做字节拷贝，data 里是 ASCII 而非 UTF-16，
 * 后续子串/拼接/wcstombs 全部按 2 字节/字符错位（00000440 拼出 ".dat"）。
 *
 * @param src     源 UTF-8 地址 (对应 r0)
 * @param src_len 源最大字节数 (对应 r1；0xFFFFFFFF = 直到 NUL)
 * @param dst     目标 UTF-16 缓冲 (对应 r2)
 * @param dst_len 目标最大字符数 (对应 r3，含结尾 NUL)
 * @return 写入的字符数（不含 NUL）
 */
uint32_t zm_strcpy(uc_engine *uc, uint32_t src, uint32_t src_len, uint32_t dst,
                   uint32_t dst_len) {
  if (!src || !dst || dst_len == 0)
    return 0;

  uint16_t *out = malloc((size_t)dst_len * 2);
  if (!out)
    return 0;

  uint32_t n = 0; /* 已写字符数 */
  uint32_t pos = 0;
  while (n + 1 < dst_len) {
    if (src_len != 0xFFFFFFFFu && pos >= src_len)
      break; /* 源长度耗尽 */
    uint8_t b = 0;
    if (uc_mem_read(uc, src + pos, &b, 1) != UC_ERR_OK)
      break;
    pos++;
    if (b == 0)
      break; /* NUL 终止 */

    uint32_t cp = 0;
    if (b < 0x80) {
      cp = b;
    } else if ((b & 0xE0) == 0xC0) {
      uint8_t b2 = 0;
      if (uc_mem_read(uc, src + pos, &b2, 1) != UC_ERR_OK)
        break;
      pos++;
      cp = ((uint32_t)(b & 0x1F) << 6) | (b2 & 0x3F);
    } else if ((b & 0xF0) == 0xE0) {
      uint8_t b2 = 0, b3 = 0;
      if (uc_mem_read(uc, src + pos, &b2, 1) != UC_ERR_OK)
        break;
      pos++;
      if (uc_mem_read(uc, src + pos, &b3, 1) != UC_ERR_OK)
        break;
      pos++;
      cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(b2 & 0x3F) << 6) |
           (b3 & 0x3F);
    } else {
      /* 非法 UTF-8 前缀：按单字节处理，避免卡死 */
      cp = b;
    }
    /* BMP 内码直接写 UTF-16；超出 BMP 的用代理对（先简化按 ? 处理） */
    if (cp <= 0xFFFF)
      out[n++] = (uint16_t)cp;
    else
      out[n++] = 0x003F; /* '?' */
  }
  out[n] = 0; /* NUL 终止 */

  uc_mem_write(uc, dst, out, (n + 1) * 2);
  free(out);
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
  /* 参数数组布局（由 00000506 sub_18C90 反推确认）：
   * applet 的 sprintf 包装（sub_18EDC）把每个可变参数展开成 8 字节槽，
   * 4 字节参数的值放在槽的 +4 偏移处（高半槽），double 占整槽。
   * 因此 arg_off 从 4 起步、每参数前进 8。旧实现每步只 +4，
   * 导致第二个及以后的 %s 读到相邻槽的 padding 垃圾，资源名全空。 */
  uint32_t arg_off = 4; /* 第一个参数值位于 args_addr + 4 */
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
      arg_off += 8; /* 参数数组每项 8 字节 */
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
      arg_off += 8; /* 参数数组每项 8 字节 */
      written = snprintf(out + oi, out_cap - oi, spec, v);
      break;
    }
    case 'c': {
      uint32_t v = 0;
      uc_mem_read(uc, args_addr + arg_off, &v, 4);
      arg_off += 8; /* 参数数组每项 8 字节 */
      written = snprintf(out + oi, out_cap - oi, spec, (int)v);
      break;
    }
    case 's': {
      uint32_t v = 0;
      uc_mem_read(uc, args_addr + arg_off, &v, 4);
      arg_off += 8; /* 参数数组每项 8 字节（见函数头注释） */
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
      /* double 占完整 8 字节槽（低 4 字节在槽首） */
      uint64_t v = 0;
      uc_mem_read(uc, args_addr + arg_off - 4, &v, 8);
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
 * @brief root.str_ctor：把源 C 字符串拷贝到客户机目标地址（含 '\0'）
 *
 * 00000102.app 中的实际用法是“扩展名替换”：
 *   strcpy_cstr(instance+0x214, "00000102.app")  // 复制完整文件名
 *   strchr(..., '.')                             // 找到 '.'
 *   strcpy_cstr('.'+1, "zmr")                    // 把 "app" 替换成 "zmr"
 *
 * @param dest_struct 目标地址（对应 r0）
 * @param src_str     源字符串地址（对应 r1）
 * @return 返回 dest_struct（即 r0）
 */
uint32_t zm_strcpy_cstr(uc_engine *uc, uint32_t dest_struct, uint32_t src_str) {
  if (dest_struct == 0 || src_str == 0)
    return dest_struct;

  char cstr[256];
  read_cstr(uc, src_str, cstr, sizeof(cstr));
  log_debug("strcpy_cstr: '%s' -> 0x%08X", cstr, dest_struct);

  uint32_t clen = (uint32_t)strlen(cstr);
  if (clen > 255)
    clen = 255;

  uc_mem_write(uc, dest_struct, cstr, clen + 1);

  return dest_struct;
}

/**
 * @brief root.spec_lookup：查格式规格
 *
 * 从 ch_addr 起读取格式说明（可能带 flags/宽度/精度/长度修饰符），
 * 找到第一个转换字符（"opusxXcdf" 集合之一），返回该字符的**地址**。
 * 未找到转换字符返回 0。
 *
 * 注意：**必须返回转换字符的地址（可能 ≠ ch_addr）**。00000506 的
 * sub_18C90 / 00000440 的 sub_3BFDC（sprintf 包装）在每次转换后执行
 * `ADD R0, R1, #1` 推进格式串指针，R1 就是本函数返回值——只有返回
 * fmt 中转换字符的位置，格式串才能正确前进；00000440 的 "%s%04d.rms"
 * 中宽度修饰符 '0' 若返回 0，整个 sprintf 会被中止（症状：rms 路径
 * 拼成 ".dat"，slg 关卡永不加载）。
 *
 * @param ch_addr 格式说明起点（对应 r0，通常是 '%' 后第一个字符）
 * @return 命中返回转换字符地址，未命中返回 0
 */
uint32_t zm_spec_lookup(uc_engine *uc, uint32_t ch_addr) {
  char buf[32] = {0, 0};
  read_cstr(uc, ch_addr, buf, sizeof(buf));
  const char *p = buf;
  /* 跳过 flags：- + 空格 # 0 */
  while (*p && strchr("-+ #0", *p))
    p++;
  /* 跳过 width：数字 */
  while (*p && *p >= '0' && *p <= '9')
    p++;
  /* 跳过 .precision */
  if (*p == '.') {
    p++;
    while (*p && *p >= '0' && *p <= '9')
      p++;
  }
  /* 跳过 length：l h z j L */
  while (*p && strchr("lhzjL", *p))
    p++;
  if (*p && strchr("opusxXcdf", *p)) {
    uint32_t off = (uint32_t)(p - buf);
    uint8_t val = (uint8_t)*p;
    uc_mem_write(uc, DUMMY_BUF, &val, 1);
    return ch_addr + off;
  }
  return 0;
}

/**
 * @brief root.str_find → zm_strchr：在字符串对象或裸 C 串中查找字符
 *
 * 先按 zmaee 字符串对象（+0=data_ptr, +4=len）解析；解析失败则把
 * str_obj_ptr 当裸 C 字符串缓冲区处理。
 * 命中返回字符在客户机中的地址，未命中返回 0。
 *
 * @param str_obj_ptr 字符串对象基址或裸 C 串地址（对应 r0）
 * @param ch          待查找字符（对应 r1，取低 8 位）
 * @return 命中返回子指针，未命中返回 0
 */
uint32_t zm_strchr(uc_engine *uc, uint32_t str_obj_ptr, uint32_t ch) {
  if (str_obj_ptr == 0)
    return 0;

  /*
   * 兼容两种形态：
   *   (a) zmaee 字符串对象：+0=data_ptr，+4=len，data_ptr 可映射。
   *   (b) 裸 C 字符串缓冲区：strcpy_cstr 把文件名直接复制到 instance+0x214
   *       后，strchr 需要能直接在缓冲区里查找字符。
   */
  uint32_t data_ptr = 0, len = 0;
  bool as_obj = false;
  if (uc_mem_read(uc, str_obj_ptr, &data_ptr, 4) == UC_ERR_OK &&
      uc_mem_read(uc, str_obj_ptr + 4, &len, 4) == UC_ERR_OK) {
    if (data_ptr == str_obj_ptr + 12) {
      as_obj = true; /* str_assign/strcpy_cstr 的内联对象布局 */
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
 *   1. data_ptr==ptr+12（strcpy_cstr/str_assign 内联布局）→ 解引用
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

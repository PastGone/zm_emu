#include "zm_str.h"

#include <stdlib.h>
#include <string.h>

#include "zm_addrs.h"
#include "zm_common.h"

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
 * @brief 格式化字符串并写入客户机目标地址（模拟简易 sprintf）
 * @param dest_addr 目标缓冲区客户机地址 (对应 r0)
 * @param fmt_addr  格式字符串客户机地址 (对应 r1)
 * @param args_addr 参数列表基址 (对应 r2)，第一个参数位于 args_addr + 4
 * @return 写入目标缓冲区的字符串长度（不含结尾 '\0'）
 *
 * @note 当前实现仅支持 %u, %d, %s 三种格式，且仅处理第一个参数。
 *       若格式串不含上述格式，则直接复制原格式串。
 */
uint32_t zm_sprintf(uc_engine *uc, uint32_t dest_addr, uint32_t fmt_addr,
                    uint32_t args_addr) {
  // 1. 读取格式字符串到宿主机缓冲区
  char fmt[256];
  read_cstr(uc, fmt_addr, fmt, sizeof(fmt));

  // 2. 准备输出缓冲区
  char out[256];

  // 3. 根据格式类型，从参数列表（args_addr + 4）读取对应参数并格式化
  //    第一个参数实际位于 args_addr + 4（r2+4）
  uint32_t arg = 0;
  if (strstr(fmt, "%u")) {
    uc_mem_read(uc, args_addr + 4, &arg, 4);
    snprintf(out, sizeof(out), fmt, arg);
  } else if (strstr(fmt, "%d")) {
    uc_mem_read(uc, args_addr + 4, &arg, 4); // 有符号转换
    int32_t sarg = (int32_t)arg;
    snprintf(out, sizeof(out), fmt, sarg);
  } else if (strstr(fmt, "%s")) {
    // %s 时 args_addr+4 处存放的是字符串的客户机地址
    uc_mem_read(uc, args_addr + 4, &arg, 4);
    char s[256];
    read_cstr(uc, arg, s, sizeof(s));
    snprintf(out, sizeof(out), fmt, s);
  } else {
    // 不包含特殊格式，直接拷贝原字符串
    strncpy(out, fmt, sizeof(out) - 1);
    out[sizeof(out) - 1] = '\0';
  }

  // 4. 计算长度并将结果写回客户机内存（包含 '\0' 结束符）
  size_t out_len = strlen(out);
  uc_mem_write(uc, dest_addr, out, out_len + 1);

  // 5. 返回写入的字符数（不含 '\0'）
  return out_len;
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
  char cstr[256];
  read_cstr(uc, src_str, cstr, sizeof(cstr));
  uint32_t clen = (uint32_t)strlen(cstr);
  uint32_t inline_buf = dest_struct + 12;

  zm_write32(uc, dest_struct, inline_buf); // 数据指针
  zm_write32(uc, dest_struct + 4, clen);   // 长度
  zm_write32(uc, dest_struct + 8, clen);   // 容量
  uc_mem_write(uc, inline_buf, cstr, clen + 1);

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
  uint32_t cstr_ptr = zm_read32(uc, str_obj_ptr);
  char cstr[256];
  read_cstr(uc, cstr_ptr, cstr, sizeof(cstr));
  char needle = (char)(ch & 0xFF);
  char *p = strchr(cstr, needle);
  if (p) {
    return cstr_ptr + (uint32_t)(p - cstr);
  }
  return 0;
}

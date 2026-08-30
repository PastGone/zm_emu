#include "zm_str.h"

#include <stdbool.h>
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

/*
 * 已删除的死代码（其功能由 src/ulibc 完全取代，trap.c 已接到 ulibc）：
 *   zm_strcpy       → u_memcpy   （TR_root_str_copy）
 *   zm_sprintf      → u_sprintf  （TR_root_sprintf）
 *   zm_strcpy_cstr  → u_strcpy   （TR_root_str_ctor）
 * 保留本文件的其余函数，它们处理的是 zmaee 领域语义（字符串对象三元组、
 * 规格表查询），ulibc 只提供标准 C 语义，不覆盖这些。
 */

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

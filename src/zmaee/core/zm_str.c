#include "zm_str.h"

#include <stdbool.h>
#include <string.h>

#include "../../emu.h"
#include "../../log/log.h"
#include "../../ulibc/include/u_mem.h" /* u_strlen（root[0x90] zm_strlen） */

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
 * @brief root[0xA4] = zmaee_strpbrk(str, charset)：找集合中任一字符的首次出现
 *
 * 逆向证据（00000506 的 sprintf 包装 sub_98EDC 内部）：
 *   0x98CC0  add r1, pc, #0xe0     ; r1 = "dufocsxXp"（合法转换符集合）
 *   0x98CC4  bl  #0x98C7C          ; ROOT_TABLE_ADDR[0xA4](r0=格式串当前位置, r1=集合)
 *   0x98CD0  mov r1, r0
 *   0x98CD4  ldrb r0, [r0]         ; 读**返回指针处**的字符当转换符
 * 调用方把返回值当作"格式串内的地址"继续 `r1+1` 扫描，因此必须返回
 * 原串内部的地址（strpbrk 语义），不能返回宿主侧临时缓冲——旧实现返回
 * DUMMY_BUF 会让 applet 的扫描指针跳到假缓冲区，导致变参个数被少算
 * （"%s\\%s" 只数出 1 个参数，路径只拼出 "res\\"）。
 *
 * 兼容性：旧的"单字符查询"用法（r0 指向 1 字符的串）在 strpbrk 语义下
 * 行为等价——命中仍返回该字符地址，调用方读 *ret 得到同一个字符。
 *
 * @param str_addr     待扫描的字符串（对应 r0）
 * @param charset_addr 字符集合，NUL 结尾（对应 r1）；为 0 时用默认转换符集
 * @return 命中返回该字符在客户机中的地址；未命中返回 0
 */
uint32_t zm_spec_lookup(uc_engine *uc, uint32_t str_addr,
                        uint32_t charset_addr) {
  if (str_addr == 0)
    return 0;

  char set[64];
  if (charset_addr)
    read_cstr(uc, charset_addr, set, sizeof(set));
  else
    set[0] = '\0';
  if (set[0] == '\0')
    snprintf(set, sizeof(set), "dufocsxXp");

  for (uint32_t i = 0;; i++) {
    uint8_t c = 0;
    if (uc_mem_read(uc, str_addr + i, &c, 1) != UC_ERR_OK)
      return 0;
    if (c == 0)
      return 0;
    if (strchr(set, (char)c))
      return str_addr + i;
  }
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
 * @brief 把"可能是 zmaee 字符串对象"的客户机地址解析成真正的字符数据地址
 * @return 解析后的地址（原样返回表示它就是裸 C 串）
 */
static uint32_t resolve_str_ptr(uc_engine *uc, uint32_t p) {
  if (p == 0)
    return 0;
  uint32_t data_ptr = 0, len = 0;
  if (uc_mem_read(uc, p, &data_ptr, 4) == UC_ERR_OK &&
      uc_mem_read(uc, p + 4, &len, 4) == UC_ERR_OK) {
    if (data_ptr == p + 12)
      return data_ptr; /* str_assign/str_cpy_cstr 的内联对象布局 */
    if (data_ptr != 0 && len < 4096) {
      uint8_t first = 0;
      if (uc_mem_read(uc, data_ptr, &first, 1) == UC_ERR_OK &&
          (first == 0 || (first >= 0x20 && first < 0x80)))
        return data_ptr;
    }
  }
  return p;
}

/**
 * @brief root[0xB0] → zmaee_strstr(haystack, needle)：子串查找
 *
 * 逆向依据（00000506 sub_8A20 资源加载分支）：
 *   r1 = 字面量 ".zbmp"
 *   bl  sub_190B0                 ; ROOT_TABLE_ADDR[0xB0](文件名, ".zbmp")
 *   cmp r0, #0
 *   bne <走 .zbmp 原生位图分支>   ; 非 0（找到）→ 按 zbmp 解
 *   ...
 *   <否则> 走 IImage/CreateImage 分支（png/jpg）
 * 即返回值是"找到则非 0"的**指针**语义 —— 正是 strstr。
 *
 * 之前该槽未实现，恒返回 0，导致 .zbmp 资源被误判成 png 走 CreateImage
 * 分支；而那条分支依赖 r7+0x58 的 display 对象（此时尚未创建），
 * 于是 `ldr r0,[r7,#0x58]` 取到 0 → 解引用野指针 → PC 飞到垃圾地址崩溃。
 *
 * @return 命中返回子串在客户机中的起始地址；未命中返回 0
 */
uint32_t zm_strstr(uc_engine *uc, uint32_t haystack, uint32_t needle) {
  uint32_t hp = resolve_str_ptr(uc, haystack);
  uint32_t np = resolve_str_ptr(uc, needle);
  if (hp == 0 || np == 0)
    return 0;

  char nbuf[64];
  read_cstr(uc, np, nbuf, sizeof(nbuf));
  size_t nl = strlen(nbuf);
  if (nl == 0)
    return hp;

  char hbuf[512];
  read_cstr(uc, hp, hbuf, sizeof(hbuf));

  char *hit = strstr(hbuf, nbuf);
  if (!hit)
    return 0;
  return hp + (uint32_t)(hit - hbuf);
}

/**
 * @brief root[0x90] → zm_strlen：字符串长度（兼容字符串对象 / 裸 C 串）
 *
 * 逆向证据（applet 调用现场，均为"长度"用法）：
 *   00000506: sub_313C 中 sprintf 出 "res\\xxx.png" 后取长度，作为
 *             IImage::SetData(0, name, len) 的 len；img->vt[8] 用它读文件。
 *   00001b62: 0xB7DAC 取长度后 +1 与缓冲容量比较，再调用 strcpy 家族；
 *             0xB91C0 取长度后 and 0xFF 当字节长度用。
 * 之前当作 strchr 是误判：那些调用点的 r1 其实是未定义的残留值。
 */
uint32_t zm_strlen(uc_engine *uc, uint32_t str_obj_ptr) {
  if (str_obj_ptr == 0)
    return 0;

  /* 与 zm_strchr 同款形态判定：字符串对象（+0=data_ptr,+4=len）或裸 C 串 */
  uint32_t data_ptr = 0, len = 0;
  bool as_obj = false;
  if (uc_mem_read(uc, str_obj_ptr, &data_ptr, 4) == UC_ERR_OK &&
      uc_mem_read(uc, str_obj_ptr + 4, &len, 4) == UC_ERR_OK) {
    if (data_ptr == str_obj_ptr + 12) {
      as_obj = true; /* str_assign/str_cpy_cstr 的内联对象布局 */
    } else if (data_ptr != 0 && len < 4096) {
      uint8_t first = 0;
      if (uc_mem_read(uc, data_ptr, &first, 1) == UC_ERR_OK &&
          (first == 0 || (first >= 0x20 && first < 0x80))) {
        as_obj = true;
      }
    }
  }

  if (as_obj)
    return len;

  return u_strlen(uc, str_obj_ptr);
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

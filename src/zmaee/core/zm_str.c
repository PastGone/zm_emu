#include "zm_str.h"

#include <stdbool.h>
#include <string.h>

#include "../../emu.h"
#include "../../log/log.h"
#include "../../tool/uc_helper.h" /* uc_write16（UCS-2 目标写入） */
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
 *   zm_sprintf      → u_sprintf  （TR_root_sprintf）
 *   zm_strcpy_cstr  → u_strcpy   （TR_root_str_ctor）
 * 保留本文件的其余函数，它们处理的是 zmaee 领域语义（字符串对象三元组、
 * 规格表查询、窄⇄宽字符转换），ulibc 只提供标准 C 语义，不覆盖这些。
 */

/**
 * @brief root[0x20] = ZMAEE_Utf8_2_Ucs2：UTF-8 → UCS-2 转换拷贝
 *
 * 真机反编译（ZMAEE_Utf8_2_Ucs2(a1=utf8, a2=utf8字节数, a3=ucs2目标,
 * a4=目标字符容量)）：
 *   - 逐字符解码 1 / 2 / 3 字节 UTF-8 写进 _WORD 目标；
 *   - 其余前导（含 4 字节序列）一律写 0xFFFF 并**跳过 5 字节**；
 *   - 输入读尽（a2 用满）或"下一格就是收尾 NUL"（a4 == 已写数+1）即停；
 *   - 结尾一定在 a3 + 2*写入字符数 处补一个 0；
 *   - 返回 **R0 = 写入的字符数**（真机同时返回 R1 = 2*字符数，即字节数；
 *     模拟器的 trap 只回写 R0，实测调用方只读 R0）。
 *
 * 【2026-09 正名】这个槽以前被误判成 `str_copy`（按长度 memcpy、src 在前）。
 * 误判能"看着能用"是因为当时的样本全是 ASCII，memcpy 恰好把字节原样搬过去。
 * 真值由 00000102（数字键盘）坐实：它 `sprintf("%u")` 出窄串 → 本槽转换 →
 * 把**返回的字符数**当 IDisplay::DrawText 的 len 用，缓冲实测为 UCS-2；
 * 而 applet 镜像里的字面量（"%u" / "zmr"）全是 ASCII，与 a2=源字节数吻合。
 *
 * @param src        UTF-8 源（客户机地址，对应 r0）
 * @param src_bytes  源字节数（r1）
 * @param dst        UCS-2 目标（r2）
 * @param dst_words  目标**字符**容量（r3，含收尾 NUL 的位置）
 * @return 写入的 UCS-2 字符数（不含收尾 NUL）
 */
uint32_t zm_utf8_to_ucs2(uc_engine *uc, uint32_t src, uint32_t src_bytes,
                         uint32_t dst, uint32_t dst_words) {
  uint8_t b0 = 0, b1, b2;
  uint32_t written = 0; /* 已写入的 UCS-2 字符数（= 返回值） */
  uint32_t i = 0;       /* 当前字符的输入字节下标 */
  uint32_t next;        /* 下一个字符的输入字节下标 */

  if (!src || !dst || src_bytes == 0 || dst_words <= 1 ||
      uc_mem_read(uc, src, &b0, 1) != UC_ERR_OK || b0 == 0) {
    uc_write16(uc, dst, 0); /* 真机：早退也要在目标开头补 NUL */
    return 0;
  }

  for (;;) {
    if ((b0 & 0x80) == 0) { /* 1 字节 */
      if (src_bytes < i + 1)
        break;
      uc_write16(uc, dst + 2 * written, b0);
      next = i + 1;
    } else if ((b0 & 0xE0) == 0xC0) { /* 2 字节 */
      if (src_bytes < i + 2)
        break;
      uc_mem_read(uc, src + i + 1, &b1, 1);
      uc_write16(uc, dst + 2 * written,
                 (uint16_t)(((b0 & 0x3F) << 6) | (b1 & 0x3F)));
      next = i + 2;
    } else if ((b0 & 0xF0) == 0xE0) { /* 3 字节 */
      if (src_bytes < i + 3)
        break;
      uc_mem_read(uc, src + i + 1, &b1, 1);
      uc_mem_read(uc, src + i + 2, &b2, 1);
      uc_write16(uc, dst + 2 * written,
                 (uint16_t)((b0 << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F)));
      next = i + 3;
    } else { /* 非法前导（含 4 字节序列）：写 0xFFFF 并跳 5 字节（真机行为） */
      uc_write16(uc, dst + 2 * written, 0xFFFF);
      next = i + 5;
      written++;
      if (src_bytes <= next)
        break;
      goto next_char;
    }
    written++;
    if (src_bytes <= next)
      break; /* 输入读尽 */

  next_char:
    if (uc_mem_read(uc, src + next, &b0, 1) != UC_ERR_OK)
      break;
    if (b0 == 0 || dst_words == written + 1)
      break; /* 遇 '\0'，或再写一格就没位置放收尾 NUL */
    i = next;
  }

  uc_write16(uc, dst + 2 * written, 0); /* 真机：结尾一定补 NUL */
  return written;
}

/**
 * @brief root[0x24] = ZMAEE_Ucs2_2_Utf8：UCS-2 → UTF-8 转换拷贝
 *
 * 与 root[0x20]（zm_utf8_to_ucs2）互为反向。真机反编译
 * （ZMAEE_Ucs2_2_Utf8(a1=ucs2源, a2=源**字符**数, a3=utf8目标,
 * a4=目标**字节**容量)，见参考 libaee.so.c.txt:52664）：
 *   - 逐个取 16 位码元，按 1 / 2 / 3 字节 UTF-8 编码写进目标；
 *   - 每格先判容量 a4：**装不下就停**（注意是 `a4 <= 已写+需要`，即要给
 *     收尾 NUL 留位置）；超过 0x7FF 的码元按 3 字节编（不做代理对处理）；
 *   - 输入读尽（a2 个字符用完）或读到 0 码元即停；
 *   - 结尾一定在 a3 + 写入字节数 处补一个 0；
 *   - 返回 **R0 = 写入的字节数**（不含收尾 NUL）。
 *
 * 实测（00000502）：sub_1CD78 用它把对象里 11 个字符的宽串
 * （r0=源, r1=0xB, r2=栈缓冲, r3=0x40）转成窄串后与字面量比较。
 *
 * @param src        UCS-2 源（客户机地址，r0）
 * @param src_chars  源**字符**数（r1）
 * @param dst        UTF-8 目标（r2）
 * @param dst_bytes  目标**字节**容量（r3，含收尾 NUL 的位置）
 * @return 写入的 UTF-8 字节数（不含收尾 NUL）
 */
uint32_t zm_ucs2_to_utf8(uc_engine *uc, uint32_t src, uint32_t src_chars,
                         uint32_t dst, uint32_t dst_bytes) {
  uint16_t w = 0;
  uint32_t written = 0; /* 已写入的字节数（= 返回值） */

  if (!src || !dst || src_chars == 0 ||
      uc_mem_read(uc, src, &w, 2) != UC_ERR_OK || w == 0) {
    uc_write16(uc, dst, 0); /* 真机：早退也在目标开头补 NUL */
    return 0;
  }

  for (uint32_t i = 0;; i++) {
    uint8_t out[3];
    uint32_t n;
    if (w <= 0x7F) {
      if (dst_bytes <= written + 1)
        break;
      out[0] = (uint8_t)w;
      n = 1;
    } else if (w <= 0x7FF) {
      if (dst_bytes <= written + 2)
        break;
      out[0] = (uint8_t)((w >> 6) | 0xC0);
      out[1] = (uint8_t)((w & 0x3F) | 0x80);
      n = 2;
    } else {
      if (dst_bytes <= written + 3)
        break;
      out[0] = (uint8_t)((w >> 12) | 0xE0);
      out[1] = (uint8_t)(((w >> 6) & 0x3F) | 0x80);
      out[2] = (uint8_t)((w & 0x3F) | 0x80);
      n = 3;
    }
    uc_mem_write(uc, dst + written, out, n);
    written += n;
    if (src_chars <= i + 1)
      break; /* 源读尽 */
    if (uc_mem_read(uc, src + 2 * (i + 1), &w, 2) != UC_ERR_OK)
      break;
    if (w == 0)
      break;
  }

  if (getenv("ZM_LOG_UCS2")) {
    char out[64] = {0};
    char in[64] = {0};
    uint32_t n = written < 40 ? written : 40;
    uc_mem_read(uc, dst, out, n);
    uc_mem_read(uc, src, in, 40);
    char hex[3 * 24 + 1];
    for (int k = 0; k < 24; k++) snprintf(hex + k * 3, 4, "%02X ", (uint8_t)in[k]);
    log_info("[UCS2]   src 原始 24 字节: %s", hex);
    log_info("[UCS2] src=0x%X chars=%u first_w=0x%X -> dst=0x%X bytes=%u \"%s\"",
             src, src_chars, (unsigned)((uint8_t)in[0] | ((uint8_t)in[1] << 8)),
             dst, written, out);
  }
  uc_write16(uc, dst + written, 0); /* 真机：结尾一定补 NUL（只写 1 字节即可） */
  return written;
}

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
 * @brief root[0xD8] = zmaee_wcslen：**宽字符串（UCS-2）长度，返回字符数**
 *
 * 【2026-09 正名】这个槽以前被记成 `GetTickCount`（返回 SDL_GetTicks），
 * 是早前的猜测。实测 5 处调用点全是"取长度"、没有一处像时间：
 *   00000506 sub_19178：返回值直接当 IDisplay::DrawText 的 len（文本是 UCS-2）
 *   00000440 sub_3C4C4：同上（`LDR R0,[R4,#0x64]` → 长度 → DrawText(text=[R4+0x64])）
 *   0000050b：`BL 0xD8` → `MOV R0,R0,LSL#1` —— **×2 = 字符数→字节数**
 *   00000001 sub_10920：同样 `LSL#1` 后当长度用
 *   00000001 0xF49C：`MUL R2,R0,R5`（长度 × 个数，布局用）
 * `LSL#1` 这条是决定性的：只有"字符数"才需要乘 2 换成字节。
 * GetTick 在别处另有来源（IShell 的 +0x48 等），与本槽无关。
 *
 * @param ptr UCS-2 字符串（客户机地址）
 * @return 字符数（不含收尾 NUL）；ptr 为 0 返回 0
 */
uint32_t zm_wcslen(uc_engine *uc, uint32_t ptr) {
  if (!ptr)
    return 0;
  uint32_t n = 0;
  for (; n < 0x10000u; n++) { /* 上限防御：脏指针不至于死循环 */
    uint8_t b[2] = {0, 0};
    if (uc_mem_read(uc, ptr + n * 2u, b, 2) != UC_ERR_OK)
      break;
    if (b[0] == 0 && b[1] == 0)
      break;
  }
  return n;
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

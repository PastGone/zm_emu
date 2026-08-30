
#include <string.h>

#include "../../include/internal/u_str_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_str_impl.c
 * @brief 字符串内部共享的字符集工具（非公开函数）
 */

void u_str_build_set(uc_engine *uc, uint32_t set, uint8_t *tbl, size_t tblsz) {
  memset(tbl, 0, tblsz);
  if (!uc || set == 0)
    return;
  uint32_t len = u_strlen(uc, set);
  uint8_t buf[64];
  for (uint32_t off = 0; off < len;) {
    uint32_t c = len - off;
    if (c > (uint32_t)sizeof(buf))
      c = (uint32_t)sizeof(buf);
    if (!u_read(uc, set + off, buf, c))
      break;
    for (uint32_t i = 0; i < c; i++)
      tbl[buf[i] >> 3] |= (uint8_t)(1u << (buf[i] & 7u));
    off += c;
  }
}

int u_str_set_has(const uint8_t *tbl, uint8_t c) {
  return (tbl[c >> 3] >> (c & 7u)) & 1u;
}

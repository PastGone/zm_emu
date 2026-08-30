#include "../../include/u_str.h"

#include "../../include/internal/u_str_impl.h"
#include "../../include/u_str_ext.h"
#include "../../include/u_mem.h"

/**
 * @file u_strtok.c
 * @brief u_strtok / u_strtok_reset —— 按分隔符集合切分
 *
 * 标准 strtok 的游标是隐藏的静态状态；客户机侧的静态区难以约定，
 * 故游标保存在宿主侧。u_strtok_reset 是非标准补充（见 u_str_ext.h）。
 */

static uint32_t s_tok_addr;
static int s_tok_active;

void u_strtok_reset(void) {
  s_tok_addr = 0;
  s_tok_active = 0;
}

uint32_t u_strtok(uc_engine *uc, uint32_t s, uint32_t sep) {
  if (!uc)
    return 0;

  uint32_t base;
  if (s != 0) {
    base = s;
    s_tok_active = 1;
  } else {
    if (!s_tok_active)
      return 0;
    base = s_tok_addr;
  }
  if (base == 0) {
    s_tok_active = 0;
    return 0;
  }

  uint8_t tbl[32];
  u_str_build_set(uc, sep, tbl, sizeof(tbl));

  /* 跳过前导分隔符 */
  for (;;) {
    uint8_t c = u_rd8(uc, base);
    if (c == 0) {
      s_tok_active = 0;
      s_tok_addr = 0;
      return 0;
    }
    if (!u_str_set_has(tbl, c))
      break;
    base++;
  }

  /* 找到分隔符终点 */
  uint32_t end = base;
  for (;;) {
    uint8_t c = u_rd8(uc, end);
    if (c == 0) {
      u_wr8(uc, end, 0);
      s_tok_addr = 0;
      s_tok_active = 0;
      return base;
    }
    if (u_str_set_has(tbl, c)) {
      u_wr8(uc, end, 0);
      s_tok_addr = end + 1;
      s_tok_active = 1;
      return base;
    }
    end++;
  }
}

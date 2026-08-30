#include "../../include/u_mem.h"

/**
 * @file u_strlen.c
 * @brief u_strlen —— 客户机 C 串长度
 *
 * 一切字符串函数的一级基本函数。遇未映射内存即安全停止，不会死循环。
 */

uint32_t u_strlen(uc_engine *uc, uint32_t s) {
  if (!uc || s == 0)
    return 0;

  uint8_t buf[64];
  uint32_t n = 0;
  for (;;) {
    if (!u_read(uc, s + n, buf, sizeof(buf))) {
      /* 撞到未映射内存：退回逐字节模式，把可读部分扫完 */
      for (;;) {
        if (!u_read(uc, s + n, buf, 1))
          return n;
        if (buf[0] == 0)
          return n;
        n++;
      }
    }
    for (size_t i = 0; i < sizeof(buf); i++) {
      if (buf[i] == 0)
        return n + (uint32_t)i;
    }
    n += (uint32_t)sizeof(buf);
  }
}

#include "../../include/u_stdlib.h"

#include <ctype.h>

#include "../../include/internal/u_conv_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_conv_impl.c
 * @brief 字符串→整数转换的共享扫描器（非公开函数）
 */

/** 读取客户机串 s 的第 i 个字节 */
static int gch(uc_engine *uc, uint32_t s, uint32_t i, uint8_t *out) {
  return u_read(uc, s + i, out, 1) ? 1 : 0;
}

static int digit_val(uint8_t c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z')
    return c - 'A' + 10;
  return -1;
}

uint64_t u_conv_scan_uint(uc_engine *uc, uint32_t s, int base,
                          uint32_t *end_out, int *neg_out, int *overflow_out) {
  uint32_t i = 0;
  uint8_t c = 0;
  uint64_t acc = 0;
  int any = 0;
  int neg = 0;
  int overflow = 0;

  u_errno = 0;
  if (end_out)
    *end_out = s;
  if (neg_out)
    *neg_out = 0;
  if (overflow_out)
    *overflow_out = 0;
  if (!uc || s == 0)
    return 0;
  if (base < 0 || base == 1 || base > 36)
    return 0;

  if (!gch(uc, s, i, &c))
    return 0;
  while (isspace((unsigned char)c)) {
    i++;
    if (!gch(uc, s, i, &c)) {
      if (end_out)
        *end_out = s + i;
      return 0;
    }
  }

  if (c == '+' || c == '-') {
    neg = (c == '-');
    i++;
    if (!gch(uc, s, i, &c)) {
      if (end_out)
        *end_out = s + i;
      return 0;
    }
  }

  /*
   * 前缀处理。要点：前缀里的 '0' **本身就是一个合法数字**，
   * 必须置 any=1（值为 0），否则 "0" / "0x" 这类输入会被判为"无转换"，
   * 使 endptr 错误地回退到串首（glibc 语义：strtol("0",&e,0) 的 e 在 +1）。
   */
  int b = base;
  if (b == 0) {
    b = 10;
    if (c == '0') {
      uint32_t back = i + 1u;
      uint8_t c2 = 0;
      b = 8;
      i++;
      any = 1;
      acc = 0;
      if (gch(uc, s, i, &c2) && (c2 == 'x' || c2 == 'X')) {
        uint8_t c3 = 0;
        if (gch(uc, s, i + 1u, &c3) && digit_val(c3) >= 0 &&
            digit_val(c3) < 16) {
          b = 16;
          i += 1u;
          c = c3;
        } else {
          if (end_out)
            *end_out = s + back;
          return 0;
        }
      } else {
        c = c2;
      }
    }
  } else if (b == 16 && c == '0') {
    uint8_t c2 = 0;
    if (gch(uc, s, i + 1u, &c2) && (c2 == 'x' || c2 == 'X')) {
      uint8_t c3 = 0;
      if (gch(uc, s, i + 2u, &c3) && digit_val(c3) >= 0 &&
          digit_val(c3) < 16) {
        i += 2u;
        c = c3;
        any = 1;
        acc = 0;
      }
    }
  }

  for (;;) {
    int v = digit_val(c);
    if (v < 0 || v >= b)
      break;
    any = 1;
    if (acc > (UINT64_MAX - (uint64_t)v) / (uint64_t)b)
      overflow = 1;
    else
      acc = acc * (uint64_t)b + (uint64_t)v;
    i++;
    if (!gch(uc, s, i, &c))
      break;
  }

  if (!any) {
    if (end_out)
      *end_out = s;
    return 0;
  }
  if (end_out)
    *end_out = s + i;
  if (neg_out)
    *neg_out = neg;
  if (overflow_out)
    *overflow_out = overflow;
  return acc;
}

#include "../../include/u_stdlib.h"

/**
 * @file u_div.c
 * @brief u_div / u_ldiv —— 同时求商与余数
 */

u_div_t u_div(int num, int den) {
  u_div_t r;
  r.quot = num / den;
  r.rem = num % den;
  return r;
}

u_ldiv_t u_ldiv(long num, long den) {
  u_ldiv_t r;
  r.quot = num / den;
  r.rem = num % den;
  return r;
}

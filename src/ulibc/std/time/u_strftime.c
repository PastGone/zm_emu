#include "../../include/u_time.h"

#include <stdlib.h>
#include <time.h>

#include "../../include/internal/u_time_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_strftime.c
 * @brief u_strftime —— 按客户机格式串格式化时间
 */

uint32_t u_strftime(uc_engine *uc, uint32_t dst, uint32_t max,
                    uint32_t fmt_ptr, uint32_t tm_ptr) {
  if (!uc || dst == 0 || max == 0)
    return 0;

  char fmt[256];
  u_read_cstr(uc, fmt_ptr, fmt, sizeof(fmt));

  struct tm t;
  u_tm_load(uc, tm_ptr, &t);

  uint32_t cap = max > 4096u ? 4096u : max;
  char *buf = (char *)malloc((size_t)cap + 1u);
  if (!buf)
    return 0;

  size_t n = strftime(buf, (size_t)cap + 1u, fmt, &t);
  if (n > 0) {
    size_t w = n;
    if (w > (size_t)max)
      w = (size_t)max;
    u_write(uc, dst, buf, w);
  }
  free(buf);
  return (uint32_t)n;
}

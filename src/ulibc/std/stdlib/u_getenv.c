#include "../../include/u_stdio.h"

#include <stdlib.h>
#include <string.h>

#include "../../include/u_mem.h"

/**
 * @file u_getenv.c
 * @brief u_getenv —— 查询环境变量
 *
 * 名字串在客户机内存；返回值写入客户机缓冲区（用 snprintf 语义，
 * 超长则截断）。找不到返回 0。
 */

uint32_t u_getenv(uc_engine *uc, uint32_t name, uint32_t buf, uint32_t buflen) {
  if (!uc || name == 0)
    return 0;

  char nbuf[256];
  u_read_cstr(uc, name, nbuf, sizeof(nbuf));

  const char *v = getenv(nbuf);
  if (!v)
    return 0;
  if (buf == 0 || buflen == 0)
    return (uint32_t)(strlen(v) + 1); /* 只返回所需长度 */

  size_t n = strlen(v);
  size_t w = n + 1;
  if (w > (size_t)buflen)
    w = (size_t)buflen;
  u_write(uc, buf, v, w);
  return (uint32_t)w;
}

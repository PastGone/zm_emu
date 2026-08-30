#include "../../include/u_stdio.h"

#include <string.h>

#include "../../include/internal/u_stdio_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_fopen.c
 * @brief u_fopen —— 打开客户机路径指向的文件
 */

uint32_t u_fopen(uc_engine *uc, uint32_t path, uint32_t mode) {
  char pbuf[1024];
  char mbuf[32];
  if (!u_read_cstr(uc, path, pbuf, sizeof(pbuf)))
    return 0;
  u_read_cstr(uc, mode, mbuf, sizeof(mbuf));
  if (mbuf[0] == '\0')
    strcpy(mbuf, "rb");

  FILE *f = fopen(pbuf, mbuf);
  if (!f)
    return 0;
  return (uint32_t)u_stdio_alloc(f);
}

#include "../../include/u_stdio.h"

#include <errno.h>
#include <string.h>

#include "../../include/internal/u_stdio_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_perror.c
 * @brief u_perror —— 把 errno 对应的描述串（前缀为客户机串）写到 stderr
 *
 * 注意：这里用**宿主** errno。若 applet 的失败来自 ulibc 自身，
 * 应改用 u_errno —— 但 perror 的语义就是报告 C 库/系统调用错误，
 * 且宿主 errno 会先被宿主 fopen/fread 等置位，故直接用宿主的。
 */

void u_perror(uc_engine *uc, uint32_t s) {
  char buf[256];
  buf[0] = '\0';
  if (uc && s)
    u_read_cstr(uc, s, buf, sizeof(buf));
  if (buf[0])
    fprintf(stderr, "%s: %s\n", buf, strerror(errno));
  else
    fprintf(stderr, "%s\n", strerror(errno));
}

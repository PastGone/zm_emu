#include "../../include/u_stdio.h"

#include <stdlib.h>

#include "../../include/internal/u_stdio_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_fread.c
 * @brief u_fread —— 读取到客户机缓冲区（分块搬运）
 */

#define U_IO_CHUNK 4096u

uint32_t u_fread(uc_engine *uc, uint32_t dst, uint32_t size, uint32_t n,
                 uint32_t fp) {
  FILE *f = u_stdio_slot(fp);
  if (!f || size == 0 || n == 0)
    return 0;

  uint32_t done = 0;
  uint8_t *buf = (uint8_t *)malloc(U_IO_CHUNK);
  if (!buf)
    return 0;

  while (done < n) {
    uint32_t batch = n - done;
    uint32_t max_item = (uint32_t)(U_IO_CHUNK / size);
    if (max_item == 0)
      max_item = 1;
    if (batch > max_item)
      batch = max_item;

    size_t got = fread(buf, (size_t)size, (size_t)batch, f);
    if (got == 0)
      break;
    if (!u_write(uc, dst + done * size, buf, (size_t)(got * size)))
      break;
    done += (uint32_t)got;
    if (got < (size_t)batch)
      break; /* EOF 或出错 */
  }
  free(buf);
  return done;
}

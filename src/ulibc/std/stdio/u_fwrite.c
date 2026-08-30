#include "../../include/u_stdio.h"

#include <stdlib.h>

#include "../../include/internal/u_stdio_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_fwrite.c
 * @brief u_fwrite —— 从客户机缓冲区写出（分块搬运）
 */

#define U_IO_CHUNK 4096u

uint32_t u_fwrite(uc_engine *uc, uint32_t src, uint32_t size, uint32_t n,
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

    if (!u_read(uc, src + done * size, buf, (size_t)(batch * size)))
      break;
    size_t wrote = fwrite(buf, (size_t)size, (size_t)batch, f);
    if (wrote == 0)
      break;
    done += (uint32_t)wrote;
    if (wrote < (size_t)batch)
      break;
  }
  free(buf);
  return done;
}

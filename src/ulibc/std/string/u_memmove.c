#include "../../include/u_mem.h"

/**
 * @file u_memmove.c
 * @brief u_memmove —— 允许 src/dst 重叠的块复制
 */

uint32_t u_memmove(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n) {
  if (!uc || n == 0)
    return 0;
  if (dst == src)
    return n;

  /* 目的区落在源区内部且偏后 → 必须从尾部倒着拷，否则会踩坏尚未读取的数据 */
  if (dst > src && dst < src + n) {
    uint8_t buf[U_MEM_CHUNK];
    uint32_t done = 0;
    while (done < n) {
      uint32_t chunk = n - done;
      if (chunk > (uint32_t)sizeof(buf))
        chunk = (uint32_t)sizeof(buf);
      uint32_t off = n - done - chunk;
      if (!u_read(uc, src + off, buf, chunk))
        break;
      if (!u_write(uc, dst + off, buf, chunk))
        break;
      done += chunk;
    }
    return done;
  }
  return u_memcpy(uc, dst, src, n);
}

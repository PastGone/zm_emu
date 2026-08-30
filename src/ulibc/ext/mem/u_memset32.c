#include "../../include/u_mem_ext.h"

#include "../../include/u_mem.h"

/**
 * @file u_memset32.c
 * @brief u_memset32 —— 按 32 位元素填充（zmaee 图形模块填 ARGB8888 显存）
 *
 * 非标准：标准 C 只有按字节的 memset。
 */

uint32_t u_memset32(uc_engine *uc, uint32_t dst, uint32_t v, uint32_t count) {
  if (!uc || count == 0)
    return dst;

  const uint32_t elems = (uint32_t)(U_MEM_CHUNK / 4u);
  uint8_t buf[U_MEM_CHUNK];
  for (uint32_t i = 0; i < elems; i++) {
    buf[i * 4u + 0u] = (uint8_t)(v & 0xFFu);
    buf[i * 4u + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    buf[i * 4u + 2u] = (uint8_t)((v >> 16) & 0xFFu);
    buf[i * 4u + 3u] = (uint8_t)((v >> 24) & 0xFFu);
  }

  uint32_t done = 0;
  while (done < count) {
    uint32_t chunk = count - done;
    if (chunk > elems)
      chunk = elems;
    if (!u_write(uc, dst + done * 4u, buf, chunk * 4u))
      break;
    done += chunk;
  }
  return dst;
}

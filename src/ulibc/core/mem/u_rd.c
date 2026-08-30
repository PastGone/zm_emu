#include "../../include/u_mem.h"

/**
 * @file u_rd.c
 * @brief u_rd8 / u_rd16 / u_rd32 —— 读定长整数（小端，与 ARM 默认端序一致）
 *
 * 三者同族且各只有几行，按 musl 惯例合并为一个翻译单元。
 */

uint8_t u_rd8(uc_engine *uc, uint32_t addr) {
  uint8_t v = 0;
  if (!u_read(uc, addr, &v, 1))
    return 0;
  return v;
}

uint16_t u_rd16(uc_engine *uc, uint32_t addr) {
  uint8_t b[2];
  if (!u_read(uc, addr, b, 2))
    return 0;
  return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

uint32_t u_rd32(uc_engine *uc, uint32_t addr) {
  uint8_t b[4];
  if (!u_read(uc, addr, b, 4))
    return 0;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

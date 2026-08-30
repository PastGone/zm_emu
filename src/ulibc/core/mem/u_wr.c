#include "../../include/u_mem.h"

/**
 * @file u_wr.c
 * @brief u_wr8 / u_wr16 / u_wr32 —— 写定长整数（小端）
 */

void u_wr8(uc_engine *uc, uint32_t addr, uint8_t v) {
  u_write(uc, addr, &v, 1);
}

void u_wr16(uc_engine *uc, uint32_t addr, uint16_t v) {
  uint8_t b[2];
  b[0] = (uint8_t)(v & 0xFFu);
  b[1] = (uint8_t)((v >> 8) & 0xFFu);
  u_write(uc, addr, b, 2);
}

void u_wr32(uc_engine *uc, uint32_t addr, uint32_t v) {
  uint8_t b[4];
  b[0] = (uint8_t)(v & 0xFFu);
  b[1] = (uint8_t)((v >> 8) & 0xFFu);
  b[2] = (uint8_t)((v >> 16) & 0xFFu);
  b[3] = (uint8_t)((v >> 24) & 0xFFu);
  u_write(uc, addr, b, 4);
}

#include "../../include/u_jmp.h"

#include "../../../log/log.h"
#include "../../include/u_mem.h"

/**
 * @file u_ctx_save.c
 * @brief u_ctx_save —— 保存客户机寄存器上下文到 jmpbuf
 *
 * 必须重写：宿主 setjmp 保存的是**宿主**寄存器，与客户机毫无关系。
 * 这里保存 AAPCS 要求被调方保存的 r4-r11，以及 sp / lr。
 */

int u_ctx_save(uc_engine *uc, uint32_t jmpbuf) {
  if (!uc || jmpbuf == 0)
    return 0;

  uint32_t vals[11];
  for (int i = 0; i < 8; i++) {
    uint64_t v = 0;
    if (uc_reg_read(uc, UC_ARM_REG_R4 + i, &v) != UC_ERR_OK) {
      log_error("u_ctx_save: 读取 R%d 失败", 4 + i);
      return 0;
    }
    vals[i] = (uint32_t)v;
  }
  {
    uint64_t sp = 0, lr = 0;
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    vals[8] = (uint32_t)sp;
    vals[9] = (uint32_t)lr;
  }
  vals[10] = U_JMP_MAGIC;

  uint8_t raw[U_JMPBUF_SIZE];
  for (int i = 0; i < 11; i++) {
    raw[i * 4 + 0] = (uint8_t)(vals[i] & 0xFFu);
    raw[i * 4 + 1] = (uint8_t)((vals[i] >> 8) & 0xFFu);
    raw[i * 4 + 2] = (uint8_t)((vals[i] >> 16) & 0xFFu);
    raw[i * 4 + 3] = (uint8_t)((vals[i] >> 24) & 0xFFu);
  }
  return u_write(uc, jmpbuf, raw, U_JMPBUF_SIZE) ? 1 : 0;
}

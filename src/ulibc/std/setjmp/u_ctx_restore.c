#include "../../include/u_jmp.h"

#include "../../../log/log.h"
#include "../../include/u_mem.h"

/**
 * @file u_ctx_restore.c
 * @brief u_ctx_restore —— 从 jmpbuf 恢复客户机寄存器（不含 R0）
 */

int u_ctx_restore(uc_engine *uc, uint32_t jmpbuf) {
  if (!uc || jmpbuf == 0)
    return 0;

  uint8_t raw[U_JMPBUF_SIZE];
  if (!u_read(uc, jmpbuf, raw, U_JMPBUF_SIZE)) {
    log_error("u_ctx_restore: 读取 jmpbuf(0x%08X) 失败", jmpbuf);
    return 0;
  }

  uint32_t vals[11];
  for (int i = 0; i < 11; i++) {
    vals[i] = (uint32_t)raw[i * 4 + 0] | ((uint32_t)raw[i * 4 + 1] << 8) |
              ((uint32_t)raw[i * 4 + 2] << 16) |
              ((uint32_t)raw[i * 4 + 3] << 24);
  }
  if (vals[10] != U_JMP_MAGIC) {
    log_error("u_ctx_restore: jmpbuf 魔数不匹配 (0x%08X)", vals[10]);
    return 0;
  }

  for (int i = 0; i < 8; i++) {
    uint64_t v = vals[i];
    if (uc_reg_write(uc, UC_ARM_REG_R4 + i, &v) != UC_ERR_OK) {
      log_error("u_ctx_restore: 写入 R%d 失败", 4 + i);
      return 0;
    }
  }
  {
    uint64_t sp = vals[8];
    uint64_t lr = vals[9];
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);
  }
  return 1;
}

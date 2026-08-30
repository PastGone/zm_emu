#include "../../include/u_mem.h"

/**
 * @file u_write.c
 * @brief u_write —— 写入客户机内存（第 0 层原语）
 */

bool u_write(uc_engine *uc, uint32_t addr, const void *buf, size_t len) {
  if (!uc || !buf)
    return false;
  if (len == 0)
    return true;
  return uc_mem_write(uc, (uint64_t)addr, buf, len) == UC_ERR_OK;
}

#include "../../include/u_wctype.h"

#include "../../include/u_mem.h"

/**
 * @file u_mbsinit.c
 * @brief u_mbsinit —— 多字节转换状态是否处于初始态
 *
 * 本实现不做真正的多字节编码转换，客户机 mbstate_t 按"首个 4 字节为 0
 * 即初始态"处理。标准规定传入 NULL 时返回非 0。
 */

int u_mbsinit(uc_engine *uc, uint32_t state_ptr) {
  if (state_ptr == 0)
    return 1;
  return u_rd32(uc, state_ptr) == 0 ? 1 : 0;
}

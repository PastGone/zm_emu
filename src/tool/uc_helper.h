#ifndef UC_HELPER_H
#define UC_HELPER_H

// -------------------- 通用内存读写辅助函数 --------------------
// 这些函数对 Unicorn 客户机内存做轻量封装，供各 trap 处理模块复用。
#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/* 从客户机地址 addr 读取一个 32 位整数（小端） */
uint32_t uc_read32(uc_engine *uc, uint32_t addr);

/* 向客户机地址 addr 写入一个 32 位整数（小端） */
uc_err uc_write32(uc_engine *uc, uint32_t addr, uint32_t val);

/* 向客户机地址 addr 写入一个 16 位整数（小端） */
uc_err uc_write16(uc_engine *uc, uint32_t addr, uint16_t val);

#endif

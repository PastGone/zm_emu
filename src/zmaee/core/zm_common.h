#ifndef __ZM_COMMON_H__
#define __ZM_COMMON_H__

// -------------------- 通用内存读写辅助函数 --------------------
// 这些函数对 Unicorn 客户机内存做轻量封装，供各 trap 处理模块复用。
#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/* 从客户机地址 addr 读取一个 32 位整数（小端） */
uint32_t zm_read32(uc_engine *uc, uint32_t addr);

/* 向客户机地址 addr 写入一个 32 位整数（小端） */
void zm_write32(uc_engine *uc, uint32_t addr, uint32_t val);

/* 从客户机地址 addr 读取 len 字节到宿主机缓冲区 buf */
void zm_read_mem(uc_engine *uc, uint32_t addr, void *buf, size_t len);

/* 将宿主机缓冲区 buf 的 len 字节写入客户机地址 addr */
void zm_write_mem(uc_engine *uc, uint32_t addr, const void *buf, size_t len);

#endif

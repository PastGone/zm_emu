#include "zm_common.h"

/* 从客户机地址 addr 读取一个 32 位整数（小端） */
uint32_t zm_read32(uc_engine *uc, uint32_t addr) {
  uint32_t val = 0;
  uc_mem_read(uc, addr, &val, 4);
  return val;
}

/* 向客户机地址 addr 写入一个 32 位整数（小端） */
void zm_write32(uc_engine *uc, uint32_t addr, uint32_t val) {
  uc_mem_write(uc, addr, &val, 4);
}

/* 从客户机地址 addr 读取 len 字节到宿主机缓冲区 buf */
void zm_read_mem(uc_engine *uc, uint32_t addr, void *buf, size_t len) {
  uc_mem_read(uc, addr, buf, len);
}

/* 将宿主机缓冲区 buf 的 len 字节写入客户机地址 addr */
void zm_write_mem(uc_engine *uc, uint32_t addr, const void *buf, size_t len) {
  uc_mem_write(uc, addr, buf, len);
}

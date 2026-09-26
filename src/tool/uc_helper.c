#include "../log/log.h"
#include <unicorn/unicorn.h>

/* 从客户机地址 addr 读取一个 32 位整数（小端） */
uint32_t uc_read32(uc_engine *uc, uint32_t addr) {
	uint32_t val = 0;
	uc_err err = uc_mem_read(uc, addr, &val, 4);
	if (err != UC_ERR_OK)
		log_error("读取错误");
	return val;
}

/* 向客户机地址 addr 写入一个 32 位整数（小端） */
uc_err uc_write32(uc_engine *uc, uint32_t addr, uint32_t val) {
	return uc_mem_write(uc, addr, &val, 4);
}

/* 向客户机地址 addr 写入一个 16 位整数（小端）。
 * UCS-2 文本操作要用（root[0x20] Utf8_2_Ucs2 / 0x18 GetType 那条线）。 */
uc_err uc_write16(uc_engine *uc, uint32_t addr, uint16_t val) {
	return uc_mem_write(uc, addr, &val, 2);
}

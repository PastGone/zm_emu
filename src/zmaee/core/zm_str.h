#ifndef __ZM_STR_H__
#define __ZM_STR_H__

// -------------------- 在这里放声明 --------------------
#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

char *read_cstr(uc_engine *uc, uint32_t addr, char *buf, size_t maxlen);
uint32_t zm_strcpy(uc_engine *uc, uint32_t src, uint32_t src_len, uint32_t dst,
                   uint32_t dst_len);
uint32_t zm_sprintf(uc_engine *uc, uint32_t dest_addr, uint32_t fmt_addr,
                    uint32_t args_addr);
#endif
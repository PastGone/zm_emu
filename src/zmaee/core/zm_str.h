#ifndef __ZM_STR_H__
#define __ZM_STR_H__

// -------------------- 字符串相关 trap 处理函数声明 --------------------
#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/* 读取客户机地址 addr 处的 C 字符串到宿主机 buf，最多 maxlen-1 字符 */
char *read_cstr(uc_engine *uc, uint32_t addr, char *buf, size_t maxlen);

/* root.str_copy：取 src_len/dst_len 较小值拷贝，返回实际拷贝字节数 */
uint32_t zm_strcpy(uc_engine *uc, uint32_t src, uint32_t src_len, uint32_t dst,
                   uint32_t dst_len);

/* root.sprintf：简易格式化（支持 %u/%d/%s），返回写入字符数（不含 '\0'） */
uint32_t zm_sprintf(uc_engine *uc, uint32_t dest_addr, uint32_t fmt_addr,
                    uint32_t args_addr);

/* root.str_ctor：把字符串封装成自描述结构体（指针+长度），写回客户机，返回结构体基址 */
uint32_t zm_str_ctor(uc_engine *uc, uint32_t dest_struct, uint32_t src_str);

/* root.spec_lookup：按单字符查规格，命中则返回存放该字符的客户机地址，否则 0 */
uint32_t zm_spec_lookup(uc_engine *uc, uint32_t ch_addr);

/* root.str_find：在字符串对象中查找字符，命中返回子指针，否则 0 */
uint32_t zm_str_find(uc_engine *uc, uint32_t str_obj_ptr, uint32_t ch);

#endif

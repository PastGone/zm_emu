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

/* root.str_ctor：把源 C 字符串拷贝到客户机目标地址（含 '\0'），返回目标地址 */
uint32_t zm_strcpy_cstr(uc_engine *uc, uint32_t dest_struct, uint32_t src_str);

/* root.spec_lookup：按单字符查规格，命中则返回存放该字符的客户机地址，否则 0 */
uint32_t zm_spec_lookup(uc_engine *uc, uint32_t ch_addr);

/* root.str_find：在字符串对象/裸 C 串中查找字符，命中返回子指针，否则 0 */
uint32_t zm_strchr(uc_engine *uc, uint32_t str_obj_ptr, uint32_t ch);

/**
 * @brief 鲁棒地读取一个"可能是 zmaee 字符串对象"的客户机地址到 buf
 *
 * 兼容两种形态：
 *   (a) zmaee 字符串对象：首 4B 为数据指针（指向内联缓冲 +12 或堆/栈地址），
 *       +4 为长度。strcpy_cstr/str_assign 构造的对象 data_ptr 通常 == ptr+12。
 *   (b) 裸 C 字符串：sprintf 拼出的 "%s%08x.app" 等直接传给 fs.open。
 *
 * 判定：先读 data_ptr=*(u32*)ptr 与 len=*(u32*)(ptr+4)。
 *   若 data_ptr==ptr+12（内联），或 data_ptr 落在已映射区间
 *   （BLOB_BASE..ZMR_BASE+ZMR_SIZE）且 len 合理（<4096）且 data_ptr
 *   处首字节可读且为可打印/0 → 按 str_obj 解引用 data_ptr。
 *   否则按裸 C 串读取 ptr。
 *
 * @return 写入 buf 的字节数（不含 '\0'）
 */
uint32_t zm_read_str_obj(uc_engine *uc, uint32_t ptr, char *buf, size_t cap);

#endif

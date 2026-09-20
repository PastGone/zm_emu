#ifndef ZM_STR_H
#define ZM_STR_H

// -------------------- 字符串相关 trap 处理函数声明 --------------------
#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/* 读取客户机地址 addr 处的 C 字符串到宿主机 buf，最多 maxlen-1 字符 */
char *read_cstr(uc_engine *uc, uint32_t addr, char *buf, size_t maxlen);

/* root.spec_lookup：按单字符查规格，命中则返回存放该字符的客户机地址，否则 0 */
/**
 * @brief root[0xA4] = zmaee_strpbrk(str, charset)
 *
 * 扫描 str，返回第一个属于 charset 的字符在客户机内存中的地址；找不到返回 0。
 * charset 为 0 时使用默认转换符集 "dufocsxXp"（applet 的 sprintf 包装用它
 * 统计变参个数）。
 */
uint32_t zm_spec_lookup(uc_engine *uc, uint32_t str_addr,
                        uint32_t charset_addr);

/* root.str_find：在字符串对象/裸 C 串中查找字符，命中返回子指针，否则 0 */
uint32_t zm_strchr(uc_engine *uc, uint32_t str_obj_ptr, uint32_t ch);

/**
 * @brief root[0x80] = zmaee_strcmp(a, b)：C 字符串比较
 *
 * 0 = 相等、非 0 = 不等（标准 strcmp）。152/176 个样本 applet 都导入该槽，
 * 属固件 libc 核心函数。原先未接线 → 默认返回 0 → 所有比较都被判成"相等"，
 * 00000442 表现为"进游戏 1 秒就弹驱蚊结束"（详见 zm_str.c 的 RE 注释）。
 */
uint32_t zm_strcmp(uc_engine *uc, uint32_t a, uint32_t b);

/**
 * @brief root[0xB0] = zmaee_strstr(haystack, needle)
 *
 * 在 haystack 中查找 needle 首次出现的位置。applet 用它判断资源文件名
 * 后缀（例如 strstr(name, ".zbmp") 决定走原生位图还是 IImage 解码）。
 *
 * @return 命中返回子串在客户机内存中的地址；未命中返回 0
 */
uint32_t zm_strstr(uc_engine *uc, uint32_t haystack, uint32_t needle);

/**
 * @brief root[0x90] → zm_strlen：字符串长度
 *
 * 兼容 zmaee 字符串对象（+0=data_ptr,+4=len）与裸 C 字符串两种形态，
 * 判定逻辑与 zm_strchr 一致。applet 侧用它取 sprintf 结果的长度。
 *
 * @return 字符串字节长度（不含 '\0'）
 */
uint32_t zm_strlen(uc_engine *uc, uint32_t str_obj_ptr);

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

/**
 * @brief root[0x20] = ZMAEE_Utf8_2_Ucs2：UTF-8 → UCS-2 转换拷贝
 *
 * 真机签名 (utf8_src, utf8字节数, ucs2_dst, dst字符容量)，返回**写入字符数**
 * （真机 R1 另回字节数 2*count，模拟器只回写 R0，实测调用方只读 R0）。
 * 与 DrawText/MeasureString 里的 Ucs2_2_Utf8 互为反向操作。
 *
 * @param src        UTF-8 源（客户机地址，对应 r0）
 * @param src_bytes  源字节数（r1）
 * @param dst        UCS-2 目标（r2）
 * @param dst_words  目标字符容量（r3，含收尾 NUL 的位置）
 * @return 写入的 UCS-2 字符数（不含收尾 NUL）
 */
uint32_t zm_utf8_to_ucs2(uc_engine *uc, uint32_t src, uint32_t src_bytes,
                         uint32_t dst, uint32_t dst_words);

/**
 * @brief root[0x24] = ZMAEE_Ucs2_2_Utf8：UCS-2 → UTF-8 转换拷贝
 *
 * 与 root[0x20] 互为反向。真机签名 (ucs2_src, 源字符数, utf8_dst,
 * 目标字节容量)，返回**写入字节数**，结尾一定补 NUL。
 * 参考反编译见 libaee.so.c.txt:52664，实现见 zm_str.c。
 *
 * @param src        UCS-2 源（客户机地址，r0）
 * @param src_chars  源字符数（r1）
 * @param dst        UTF-8 目标（r2）
 * @param dst_bytes  目标字节容量（r3，含收尾 NUL 的位置）
 * @return 写入的 UTF-8 字节数（不含收尾 NUL）
 */
uint32_t zm_ucs2_to_utf8(uc_engine *uc, uint32_t src, uint32_t src_chars,
                         uint32_t dst, uint32_t dst_bytes);

/**
 * @brief root[0xD8] = zmaee_wcslen：宽字符串（UCS-2）长度（**字符数**）
 *
 * 证据见 zm_str.c 的注释（5 处调用点皆是"取长度"，其中两处 `LSL#1` 把
 * 字符数换字节数）。此槽此前被误记为 GetTickCount。
 *
 * @return 字符数（不含收尾 NUL）；ptr 为 0 返回 0
 */
uint32_t zm_wcslen(uc_engine *uc, uint32_t ptr);

#endif

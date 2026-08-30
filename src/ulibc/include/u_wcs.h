#ifndef U_WCS_H
#define U_WCS_H
/**
 * @file u_wcs.h
 * @brief 宽字符串族 —— 标准 C（C95 修正案）
 *
 * 客户机侧按 UCS-2 / UTF-16LE（wchar_t 为 2 字节）处理，这是 ARM32
 * 嵌入式工具链（armcc / 早期 newlib）的常见配置。
 *
 * 与宿主 wchar_t（Linux 上 4 字节）宽度不同，所以**不能**直接转发宿主
 * 的 wcs* 函数 —— 必须按 2 字节元素逐个读写客户机内存。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

/** 客户机宽字符类型（2 字节，与 ARM32 嵌入式 ABI 一致） */
typedef uint16_t u_wchar;

/** 以 0x0000 结尾的 UCS-2 串长度（单位：字符） */
uint32_t u_wcslen(uc_engine *uc, uint32_t s);
uint32_t u_wcscpy(uc_engine *uc, uint32_t dst, uint32_t src);
uint32_t u_wcscat(uc_engine *uc, uint32_t dst, uint32_t src);
int u_wcscmp(uc_engine *uc, uint32_t a, uint32_t b);

/** 命中返回客户机地址，未命中返回 0 */
uint32_t u_wcschr(uc_engine *uc, uint32_t s, u_wchar c);
uint32_t u_wcsrchr(uc_engine *uc, uint32_t s, u_wchar c);

/** 子串查找：命中返回客户机地址，needle 为空串返回 s */
uint32_t u_wcsstr(uc_engine *uc, uint32_t hay, uint32_t needle);

/* -------------------- 限定长度 -------------------- */

/** wcsncpy：拷满 n 则不以 '\0' 结尾；不足则补 0（与标准一致） */
uint32_t u_wcsncpy(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n);

/** wcsncat：最多追加 n 个宽字符并补终止符 */
uint32_t u_wcsncat(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n);

int u_wcsncmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n);

/* -------------------- 扫描集合 -------------------- */

/** 返回完全由/不由 accept 中字符组成的前缀长度（单位：宽字符） */
uint32_t u_wcsspn(uc_engine *uc, uint32_t s, uint32_t accept);
uint32_t u_wcscspn(uc_engine *uc, uint32_t s, uint32_t reject);

/** 返回首个出现在 accept 中字符的客户机地址，未命中 0 */
uint32_t u_wcspbrk(uc_engine *uc, uint32_t s, uint32_t accept);

/* -------------------- 分割 -------------------- */

/** wcstok：游标保存在宿主侧；新一轮前可用 u_wcstok_reset() 复位 */
uint32_t u_wcstok(uc_engine *uc, uint32_t s, uint32_t sep);

/** 复位 u_wcstok 游标（非标准补充） */
void u_wcstok_reset(void);

/* -------------------- 宽串 → 数值 -------------------- */

long u_wcstol(uc_engine *uc, uint32_t s, uint32_t endptr_addr, int base);
unsigned long u_wcstoul(uc_engine *uc, uint32_t s, uint32_t endptr_addr,
                        int base);
double u_wcstod(uc_engine *uc, uint32_t s, uint32_t endptr_addr);

/* -------------------- wmem* 族（按宽字符计数） -------------------- */

uint32_t u_wmemcpy(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n);
uint32_t u_wmemmove(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n);
uint32_t u_wmemset(uc_engine *uc, uint32_t dst, u_wchar c, uint32_t n);
int u_wmemcmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n);

/** 返回客户机地址，未命中返回 0 */
uint32_t u_wmemchr(uc_engine *uc, uint32_t s, u_wchar c, uint32_t n);

#endif /* U_WCS_H */

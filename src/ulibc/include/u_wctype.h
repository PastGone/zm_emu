#ifndef U_WCTYPE_H
#define U_WCTYPE_H
/**
 * @file u_wctype.h
 * @brief 宽字符分类与转换 —— 标准 C（wctype.h，C95）【纯透传类】
 *
 * 参数是 wint_t（宽字符**值**）而非指针，不涉及客户机地址空间，
 * 直接转发宿主 isw* / tow*。
 *
 * 一处需注意的差异：宿主 Linux 的 wchar_t 是 4 字节（UTF-32），
 * 客户机是 2 字节（UCS-2/UTF-16）。对于 BMP（U+0000..U+FFFF）内的字符
 * 两者码点一致，直接透传结果正确；BMP 之外的码点客户机根本无法表示，
 * 故不需要特殊处理。
 *
 * wctype / wctrans / towctrans 是"按名字取分类/转换"的本地化接口，
 * 本实现固定按 C locale 处理（不做运行时 locale 查表）。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>
#include <wchar.h>	/* btowc / wctob / WEOF */
#include <wctype.h> /* isw* / tow* / wctype / wctrans */

int u_iswalnum(int wc);
int u_iswalpha(int wc);
int u_iswcntrl(int wc);
int u_iswdigit(int wc);
int u_iswgraph(int wc);
int u_iswlower(int wc);
int u_iswprint(int wc);
int u_iswpunct(int wc);
int u_iswspace(int wc);
int u_iswupper(int wc);
int u_iswxdigit(int wc);

/** 宽字符大小写转换 */
int u_towlower(int wc);
int u_towupper(int wc);

/** 宽字符 ↔ 单字节字符转换（仅 Latin-1 范围可逆，否则返回 EOF/WEOF） */
int u_wctob(int wc);
int u_btowc(int c);

/**
 * wctype / wctrans：按名字取分类/转换描述符（C95）。
 * 本实现只支持 C locale 的标准名字，未知名字返回 0。
 */
int u_iswctype(int wc, unsigned long desc);
unsigned long u_wctype(const char *name);
int u_towctrans(int wc, unsigned long desc);
unsigned long u_wctrans(const char *name);

/**
 * 多字节转换状态是否处于初始态。
 * @param state_ptr 客户机 mbstate_t 地址；传 0（NULL）时标准规定返回非 0。
 * 本实现不做真正的编码转换，按"首个 4 字节为 0 即初始态"判定。
 */
int u_mbsinit(uc_engine *uc, uint32_t state_ptr);

#endif /* U_WCTYPE_H */

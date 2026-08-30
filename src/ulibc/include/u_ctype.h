#ifndef U_CTYPE_H
#define U_CTYPE_H
/**
 * @file u_ctype.h
 * @brief 字符分类与转换 —— 标准 C（ctype.h）【纯透传类】
 *
 * 参数是 int（字符值）而非指针，不涉及客户机地址空间，也不涉及结构体布局，
 * 因此可以直接转发宿主 libc（只需要注意把负值当成 unsigned char 处理，
 * 否则 EOF(-1) 会让宿主实现产生 UB）。
 *
 * 注意：随宿主 locale 变化。模拟器若需要确定性行为，可在 std/u_ctype.c
 * 里改成自建的 256 项查表（C locale 常量表）。
 */
#include <stdint.h>

int u_isalnum(int c);
int u_isalpha(int c);
int u_iscntrl(int c);
int u_isdigit(int c);
int u_isgraph(int c);
int u_islower(int c);
int u_isprint(int c);
int u_ispunct(int c);
int u_isspace(int c);
int u_isupper(int c);
int u_isxdigit(int c);
int u_tolower(int c);
int u_toupper(int c);

#endif /* U_CTYPE_H */

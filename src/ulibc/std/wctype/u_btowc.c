#include "../../include/u_wctype.h"

/**
 * @file u_btowc.c
 * @brief u_btowc —— 单字节字符转宽字符【纯透传】
 *
 * 必须转发宿主 btowc 而不是手写 `c & 0xFF`：
 * 宿主 btowc 的结果依赖当前 locale —— 在 UTF-8 等多字节 locale 下，
 * 0x80..0xFF 这些字节不构成合法的"单字节字符"，标准规定返回 WEOF。
 * 自己按码点硬转会在这些字节上给出与宿主不一致的结果。
 */

int u_btowc(int c) {
	if (c == EOF)
		return (int)WEOF;
	return (int)btowc(c);
}

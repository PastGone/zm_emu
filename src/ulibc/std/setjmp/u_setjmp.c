#include "../../include/u_jmp.h"

/**
 * @file u_setjmp.c
 * @brief u_setjmp —— 保存上下文（依赖 u_ctx_save）
 *
 * 固定返回 0，与标准 setjmp 首次返回语义一致。
 */

int u_setjmp(uc_engine *uc, uint32_t jmpbuf) {
	u_ctx_save(uc, jmpbuf);
	return 0;
}

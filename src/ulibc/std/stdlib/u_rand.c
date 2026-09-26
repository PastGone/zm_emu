#include "../../include/u_stdlib.h"

/**
 * @file u_rand.c
 * @brief u_srand / u_rand —— 确定性伪随机（LCG）
 *
 * **不转发宿主 rand**：模拟器需要可复现的执行轨迹，
 * 而宿主 glibc 的 rand() 序列不受我们控制。
 */

static uint32_t s_seed = 1u;

void u_srand(uint32_t seed) {
	s_seed = seed ? seed : 1u;
}

int u_rand(void) {
	s_seed = s_seed * 1103515245u + 12345u;
	return (int)((s_seed >> 16) & 0x7FFFu);
}

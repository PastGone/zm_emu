#include "../../include/u_stdio.h"

#include <stdlib.h>

#include "../../include/u_mem.h"

/**
 * @file u_system.c
 * @brief u_system —— 执行宿主 shell 命令（命令串在客户机内存）
 *
 * 安全提示：这会真的在宿主机上执行命令。若模拟器需要处理不可信
 * applet，应在宿主侧禁用本函数（返回 -1）。
 */

int u_system(uc_engine *uc, uint32_t cmd) {
	if (!uc || cmd == 0)
		return -1;
	char cbuf[1024];
	if (!u_read_cstr(uc, cmd, cbuf, sizeof(cbuf)))
		return -1;
	return system(cbuf);
}

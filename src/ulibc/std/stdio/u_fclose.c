#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_fclose.c
 * @brief u_fclose —— 关闭文件；标准流不真正关闭
 */

int u_fclose(uc_engine *uc, uint32_t fp) {
	(void)uc;
	FILE *f = u_stdio_slot(fp);
	if (!f)
		return EOF;
	if (fp < 4)
		return 0;
	u_stdio_g_files[fp] = NULL;
	return fclose(f);
}

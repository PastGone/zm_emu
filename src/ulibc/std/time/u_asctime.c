#include "../../include/u_time.h"

#include <string.h> /* strlen / memcpy：把 asctime 的结果拷进本函数缓冲 */
#include <time.h>

#include "../../include/internal/u_time_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_asctime.c
 * @brief u_asctime —— struct tm 转固定格式串（依赖 u_tm_load）
 *
 * 【为什么不用 asctime_r】
 * asctime_r 是 **POSIX 扩展，不是标准 C**：Windows 工具链（MinGW/MSVC）上
 * 没有声明，编译直接报错
 *     error: implicit declaration of function 'asctime_r';
 *            did you mean 'asctime'? [-Wimplicit-function-declaration]
 * （C23 / -Werror 下必然是 error；即使侥幸链接通过，返回 `char*` 被当 `int`
 * 接也是未定义行为）。
 *
 * 换成标准 C 的 asctime：它返回**宿主静态缓冲**，本函数紧跟着就拷走 ——
 * 两条语句之间不会再有人调 asctime/ctime 覆盖它（模拟器单线程），
 * 所以这里不需要可重入版本。长度上限固定（C 标准："Www Mmm dd hh:mm:ss yyyy\n"
 * 共 25 字符 + NUL），64 字节缓冲绰绰有余。
 *
 * 注：同族的 ctime_r / localtime_r / gmtime_r 在 MinGW 上**是有声明**的
 * （只缺 asctime_r），所以那三个文件保持可重入版本不动。
 */
uint32_t u_asctime(uc_engine *uc, uint32_t tm_ptr, uint32_t buf) {
  struct tm t;
  char tmp[64];
  u_tm_load(uc, tm_ptr, &t);

  const char *s = asctime(&t);
  if (!s)
    return 0; /* 与实机同语义：asctime 只在 tm 字段超出正常范围时返回 NULL */

  size_t n = strlen(s);
  if (n >= sizeof(tmp))
    n = sizeof(tmp) - 1; /* 防御：无论如何不溢出本函数的缓冲 */
  memcpy(tmp, s, n);
  tmp[n] = '\0';

  return u_write_cstr(uc, buf, tmp) ? buf : 0;
}

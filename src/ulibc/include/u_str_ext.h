#ifndef U_STR_EXT_H
#define U_STR_EXT_H
/**
 * @file u_str_ext.h
 * @brief 字符串**非标准**扩展
 *
 * 这些函数在标准 C 里没有，主要来自两块需求：
 *
 *  ① zmaee 运行时确实需要的扩展（stricmp / strnicmp / strlwr / strupr /
 *     strtrim）—— 原始固件 libc 提供，applet 会调，必须实现。
 *
 *  ② 模拟器侧的便利函数（u_strcmp_host / u_strncmp_host / u_strstr_host /
 *     u_strtok_reset）—— 让宿主代码拿客户机串直接和宿主常量比较，
 *     省去"先抓到宿主缓冲再比"的样板代码。
 *
 *  ③ POSIX 而非 C 标准的函数（u_strdup / u_strndup）—— 因 zmaee 会用到，
 *     一并放在这里。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

/* -------------------- POSIX：堆上字符串复制 -------------------- */

/** 从客户机堆复制字符串；失败返回 0 */
uint32_t u_strdup(uc_engine *uc, uint32_t s);

/** 从客户机堆复制最多 n 字符；失败返回 0 */
uint32_t u_strndup(uc_engine *uc, uint32_t s, uint32_t n);

/* -------------------- zmaee 扩展：大小写不敏感比较 -------------------- */

/** 对应 zmaee_stricmp / zmaee_strnicmp */
int u_stricmp(uc_engine *uc, uint32_t a, uint32_t b);
int u_strnicmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n);

/* -------------------- 模拟器便利：客户机串 vs 宿主常串 -------------------- */

int u_strcmp_host(uc_engine *uc, uint32_t a, const char *b);
int u_strncmp_host(uc_engine *uc, uint32_t a, const char *b, uint32_t n);

/** 与 u_strstr 相同，但 needle 是宿主常串 */
uint32_t u_strstr_host(uc_engine *uc, uint32_t hay, const char *needle);

/* -------------------- zmaee 扩展：原地修改 -------------------- */

/** 原地大小写转换，返回 s（对应 zmaee_strlwr / zmaee_strupr） */
uint32_t u_strlwr(uc_engine *uc, uint32_t s);
uint32_t u_strupr(uc_engine *uc, uint32_t s);

/** 原地去除首尾空白，返回 s（对应 zmaee_strtrim） */
uint32_t u_strtrim(uc_engine *uc, uint32_t s);

/* -------------------- 模拟器便利：strtok 游标管理 -------------------- */

/** 复位 u_strtok 的宿主侧游标（标准 strtok 无此函数） */
void u_strtok_reset(void);

#endif /* U_STR_EXT_H */

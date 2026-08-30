#ifndef U_STR_H
#define U_STR_H
/**
 * @file u_str.h
 * @brief 客户机字符串族 —— **仅标准 C 函数**
 *
 * 归类：属于「必须重写」一类 —— 参数全是**客户机指针**，宿主 libc 一旦
 * 解引用就是段错误。全部实现建立在 u_mem 的客户机读写原语之上。
 *
 * 依赖的基本函数（一级）：u_strlen / u_memcpy / u_memcmp
 * 其余（strcpy/strcmp/strcat/strstr/...）都由这三个派生。
 *
 * ⚠ 非标准扩展（stricmp / strtrim / strlwr / strupr / strdup /
 *   str*_host 等）不在此文件，见 u_str_ext.h。
 * ⚠ 宽字符函数见 u_wcs.h。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

/* -------------------- 拷贝 / 拼接 -------------------- */

/** strcpy：含 '\0' 一起拷，返回 dst */
uint32_t u_strcpy(uc_engine *uc, uint32_t dst, uint32_t src);

/** strncpy：拷满 n 则不以 '\0' 结尾（与标准一致），返回 dst */
uint32_t u_strncpy(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n);

/** strcat：返回 dst */
uint32_t u_strcat(uc_engine *uc, uint32_t dst, uint32_t src);

/** strncat：最多追加 n 字符并补 '\0'，返回 dst */
uint32_t u_strncat(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n);

/* -------------------- 比较 -------------------- */

int u_strcmp(uc_engine *uc, uint32_t a, uint32_t b);
int u_strncmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n);

/* -------------------- 查找 -------------------- */

/** 命中返回客户机地址，未命中返回 0 */
uint32_t u_strchr(uc_engine *uc, uint32_t s, int c);
uint32_t u_strrchr(uc_engine *uc, uint32_t s, int c);

/** 子串查找：命中返回客户机地址，needle 为空串返回 s */
uint32_t u_strstr(uc_engine *uc, uint32_t hay, uint32_t needle);

/** 返回第一个属于/不属于 accept 集合的位置偏移 */
uint32_t u_strspn(uc_engine *uc, uint32_t s, uint32_t accept);
uint32_t u_strcspn(uc_engine *uc, uint32_t s, uint32_t reject);

/** 返回第一个出现在 accept 中字符的客户机地址，未命中 0 */
uint32_t u_strpbrk(uc_engine *uc, uint32_t s, uint32_t accept);

/* -------------------- 分割 -------------------- */

/**
 * strtok：宿主侧保存游标（客户机侧的静态区难以约定）。
 * s 非 0 表示新一轮；s 为 0 表示继续上一轮。
 * 非线程安全，与标准一致。
 * 新一轮前可用 u_strtok_reset() 复位（非标准，见 u_str_ext.h）。
 */
uint32_t u_strtok(uc_engine *uc, uint32_t s, uint32_t sep);

#endif /* U_STR_H */

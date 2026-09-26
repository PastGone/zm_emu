#ifndef U_FMT_H
#define U_FMT_H
/**
 * @file u_fmt.h
 * @brief 格式化输出核心 —— 一级基石之一
 *
 * 【标准 C】vsnprintf / snprintf / sprintf / printf / fprintf /
 *          vprintf / vfprintf —— C89/C99 标准函数。
 * 【基建】  u_vsnprintf_core —— 非标准签名（宿主侧格式串 + u_va 游标），
 *          是上面所有标准函数的实现核心，暴雷出来供扩展模块复用。
 *
 * 为什么不能直接调宿主 printf/vsnprintf：
 *   1. va_list 布局跨 ABI 不同（见 u_arg.h）；
 *   2. 格式串里的 %s / %n 指针是**客户机地址**，宿主解引用必然崩。
 *
 * 本模块的做法（即「解析格式串 → 逐个取参 → 交给宿主做单次转换」）：
 *   · 自己解析 %[flags][width][.prec][length]conv；
 *   · 通过 u_va 从客户机寄存器/栈/内存取出对应类型的标量；
 *   · %s 先把客户机字符串搬进宿主临时缓冲，再交给 snprintf 做宽度/精度处理；
 *   · %n 把已输出字符数写回**客户机**地址；
 *   · 其余转换拼出单个 spec 交给宿主 snprintf —— 这样既复用了宿主 libc 的
 *     浮点/本地化格式化能力，又完全避开了 va_list 与地址空间的坑。
 *
 * 返回值语义与标准 snprintf 一致：返回「本应写入的字符数」（不含 '\0'），
 * 与是否被截断无关。
 */
#include <stdio.h>

#include "u_arg.h"
#include "u_mem.h"

/** 格式串读取上限（客户机格式串超过则截断，防恶意/损坏数据） */
#define U_FMT_MAX_FMT 1024u
/** 输出上限，防止客户机传入 size=0x7FFFFFFF 导致宿主分配巨量内存 */
#define U_FMT_MAX_OUT 65536u
/** %s 在未指定精度时的默认读取上限 */
#define U_FMT_STR_CAP 4096u

/* -------------------- 基建：核心格式化引擎 -------------------- */

/**
 * 把「宿主侧格式串 fmt + 变参游标 va」格式化进宿主缓冲 out。
 * @param out 可为 NULL，cap 可为 0（此时只计数不输出，用于测长）
 * @return 本应写入的字符数（不含 '\0'）
 */
int u_vsnprintf_core(uc_engine *uc, u_va *va, char *out, size_t cap, const char *fmt);

/* -------------------- 标准 C：stdio.h 格式化家族 -------------------- */

/** vsnprintf：写入客户机 dst，最多 size 字节（含 '\0'） */
int u_vsnprintf(uc_engine *uc, uint32_t dst, uint32_t size, uint32_t fmt, u_va *va);

/** snprintf：与 u_vsnprintf 同义，仅为对齐标准命名 */
int u_snprintf(uc_engine *uc, uint32_t dst, uint32_t size, uint32_t fmt, u_va *va);

/** sprintf / vsprintf：无长度限制（内部按 U_FMT_MAX_OUT 防呆） */
int u_sprintf(uc_engine *uc, uint32_t dst, uint32_t fmt, u_va *va);
int u_vsprintf(uc_engine *uc, uint32_t dst, uint32_t fmt, u_va *va);

/** 输出到宿主 FILE* */
int u_vfprintf(uc_engine *uc, FILE *fp, uint32_t fmt, u_va *va);

/** 输出到宿主 stdout */
int u_vprintf(uc_engine *uc, uint32_t fmt, u_va *va);
int u_printf(uc_engine *uc, uint32_t fmt, u_va *va);

/** 输出到客户机 FILE 句柄（见 u_stdio.h） */
int u_vfprintf_g(uc_engine *uc, uint32_t guest_fp, uint32_t fmt, u_va *va);
int u_fprintf(uc_engine *uc, uint32_t guest_fp, uint32_t fmt, u_va *va);

#endif /* U_FMT_H */

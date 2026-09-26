#ifndef ULIBC_H
#define ULIBC_H
/**
 * @file ulibc.h
 * @brief ulibc —— 面向 Unicorn(ARM32) 客户机的「半透传 / 半重写」C 运行时
 *
 * ═════════════════════════════════════════════════════════════
 * 目录结构：musl 风格 —— 一个函数一个 .c 文件
 * ═════════════════════════════════════════════════════════════
 *
 * 顶层按「是不是标准 C 函数」分三层，层内按功能域分目录，
 * 每个公开函数独占一个 .c 文件（少数同族小函数合并，已在文件头注明）。
 *
 *   include/   公开头文件（本文件已集中包含，外部只需 #include "ulibc.h"）
 *   include/internal/
 *              内部共享头：拆分到单函数后，跨翻译单元共享的
 *              状态与辅助函数放这里（对应 musl 的 src/internal/）
 *
 *   core/      【基建·非标准】标准 C 里**没有对应物**的模拟器地基。
 *               判据：把标准删掉后这些函数依然必须存在，且它们不是任何
 *               标准函数的等价实现，而是让标准函数得以实现的前提。
 *                mem/    u_read u_write u_rd u_wr u_read_cstr u_write_cstr
 *                        —— 跨地址空间的裸读写原语，一切之上
 *                heap/   u_heap_init u_heap_ensure u_heap_reset u_heap_query
 *                        u_heap_strdup (+ u_heap_impl 内部共享)
 *                        —— 客户机堆的创建/复位/诊断；真正的
 *                           malloc/free 是标准函数，在 std/stdlib/
 *                io/     u_stdio_host_file u_stdio_cleanup
 *                        (+ u_stdio_tbl 内部句柄表)
 *                        —— 客户机 FILE* 句柄表；真正的 fopen/fread
 *                           等标准函数是它的使用者，在 std/stdio/
 *                arg/    u_arg —— AAPCS 变参游标（内部紧耦合，不拆）
 *                fmt/    u_vsnprintf_core —— 格式化引擎（签名非标准：
 *                        宿主侧格式串 + u_va）；标准的 printf 家族
 *                        是它的包装层，在 std/stdio/
 *
 *   std/       【标准 C】C89 / C95 / C99 标准库函数的等价实现
 *                string/ u_strlen u_strnlen u_memcpy u_memmove u_memset
 *                        u_memcmp u_memchr
 *                        u_strcpy u_strncpy u_strcat u_strncat u_strcmp
 *                        u_strncmp u_strchr u_strrchr u_strstr u_strspn
 *                        u_strcspn u_strpbrk u_strtok (+ u_str_impl)
 *                wchar/  u_wcslen u_wcscpy u_wcscat u_wcscmp u_wcschr
 *                        u_wcsrchr u_wcsstr u_wcsncpy u_wcsncat u_wcsncmp
 *                        u_wcsspn u_wcscspn u_wcspbrk u_wcstok
 *                        u_wcstol u_wcstoul u_wcstod
 *                        u_wmemcpy u_wmemmove u_wmemset u_wmemcmp u_wmemchr
 *                wctype/ u_iswalnum ... u_iswxdigit（11 个）
 *                        u_towlower u_towupper u_towctrans
 *                        u_wctype u_iswctype u_wctrans
 *                        u_wctob u_btowc u_mbsinit
 *                ctype/  u_isalnum ... u_toupper（13 个，纯透传宿主）
 *                math/   u_sqrt u_pow ... u_modf u_frexp（22 个）
 *                stdlib/ u_malloc u_free u_calloc u_realloc
 *                        u_errno u_exit u_atexit u_abs u_div u_rand
 *                        u_qsort_host u_bsearch_host
 *                        u_getenv u_system
 *                conv/   u_strtol u_strtoul u_strtoll u_strtod
 *                        u_atoi u_atol u_atoll u_atof (+ u_conv_impl)
 *                stdio/  u_fopen u_fclose u_fread u_fwrite u_fseek u_ftell
 *                        u_feof u_fgetc u_fputc u_fgets u_fputs
 *                        u_remove u_rename
 *                        u_getc u_getchar u_putc u_putchar u_puts u_ungetc
 *                        u_ferror u_clearerr u_perror
 *                        u_vsnprintf u_vsprintf u_sprintf
 *                        u_vfprintf u_vprintf u_fprintf（格式化包装层）
 *                time/   u_time u_difftime u_localtime u_gmtime u_mktime
 *                        u_asctime u_ctime u_strftime u_clock
 *                        (+ u_time_impl)
 *                setjmp/ u_ctx_save u_ctx_restore u_setjmp u_longjmp
 *
 *   ext/       【非标准扩展】zmaee 需要或模拟器便利，标准 C 里没有
 *                mem/    u_memset16 u_memset32 u_memrchr u_memcmp_host
 *                string/ u_strdup u_strndup u_stricmp u_strnicmp
 *                        u_strlwr u_strupr u_strtrim
 *                        u_strcmp_host u_strstr_host
 *                        (+ u_str_ext_impl 内部共享)
 *                math/   u_float_to_fixed u_fixed_to_float
 *                        u_fixed_mul u_fixed_div
 *                time/   u_tick_ms（单调毫秒，SDL_GetTicks 语义）
 *                        u_clock_ms（clock 的毫秒便利版）
 *                fmt/    u_sprintf_u32（宿主数组参数）
 *
 * ═════════════════════════════════════════════════════════════
 * 问题背景
 * ═════════════════════════════════════════════════════════════
 * 原始「子腾手表」架构里，applet 与系统 libc 在**同一个地址空间**，
 * applet 可以直接拿到系统 libc 的函数指针并跳转。
 *
 * 换到 Unicorn 之后，applet 跑在**模拟内存**里，宿主 libc 跑在**真实内存**里，
 * 两者地址空间完全隔离：客户机指针宿主不能解引用，宿主指针客户机也不能用，
 * 更不可能直接跳到宿主 libc 的函数地址上（架构/ABI 还可能不同）。
 *
 * ═════════════════════════════════════════════════════════════
 * 三类函数的划分（本库的设计依据）
 * ═════════════════════════════════════════════════════════════
 *
 *  A. 只包装一层即可透传（纯标量，无指针、无结构体布局差异）
 *     数学：sqrt/pow/sin/cos/tan/asin/acos/atan/atan2/sinh/cosh/tanh/
 *           exp/log/log10/log2/fabs/floor/ceil/fmod/ldexp/hypot
 *     字符：isalnum/isalpha/iscntrl/isdigit/isgraph/islower/isprint/
 *           ispunct/isspace/isupper/isxdigit/tolower/toupper
 *     整数：abs/labs/llabs/div/ldiv
 *
 *  B. 包装一层 + 内存搬运 / 结构体翻译（核心逻辑复用宿主）
 *     文件 IO：fopen/fread/fwrite/fclose/fseek/ftell/...（缓冲区搬运）
 *     时间：time/localtime/gmtime/strftime（struct tm 逐字段翻译）
 *
 *  C. 必须重写（涉及客户机内存管理、变参 ABI、控制流）
 *     堆：malloc/calloc/realloc/free
 *     格式化：printf / sprintf / snprintf / v* 系列
 *     内存/字符串：memcpy/memmove/memset/str* 全族
 *     控制流：setjmp/longjmp
 *
 * ═════════════════════════════════════════════════════════════
 * 基本函数（一级）依赖树 —— 先实现这些，其余都是机械劳动
 * ═════════════════════════════════════════════════════════════
 *   u_read / u_write
 *     └── u_memcpy / u_memmove / u_memset / u_memcmp
 *           └── str* 全族、文件缓冲区搬运
 *   u_malloc / u_free
 *     └── calloc / realloc / strdup / strndup
 *   u_strlen
 *     └── strcpy/strcmp/strcat/strchr/strstr/...
 *   u_vsnprintf_core（格式串解析 + u_va 取参）
 *     └── printf / sprintf / snprintf / fprintf / v* 全家族
 *   u_va（变参游标）
 *     └── 所有不定参数函数的取参
 *   u_ctx_save / u_ctx_restore
 *     └── setjmp / longjmp
 *
 * ═════════════════════════════════════════════════════════════
 * 典型用法（在 trap 处理器里）
 * ═════════════════════════════════════════════════════════════
 * @code
 *   // 例 1：ROOT[0x08] malloc —— 固定参数，直接透传
 *   case TR_root_malloc:
 *       ret = u_malloc(uc, r0);
 *       break;
 *
 *   // 例 2：sprintf(dst, fmt, ...) —— 不定参数，用 u_va 取参
 *   case TR_root_sprintf: {
 *       u_va va;
 *       u_va_start_regs(&va, uc, 2);     // 2 个固定参数：dst, fmt
 *       ret = (uint32_t)u_sprintf(uc, r0, r1, &va);
 *       break;
 *   }
 *
 *   // 例 3：vprintf(fmt, va_list) —— 客户机已把参数溢出到栈帧
 *   case TR_vprintf: {
 *       u_va va;
 *       u_va_start_va(&va, uc, r1);      // r1 = &va_list
 *       ret = (uint32_t)u_vprintf(uc, r0, &va);
 *       break;
 *   }
 *
 *   // 例 4：参数已在宿主数组里（老 trap 处理器）
 *   uint32_t args[2] = {name_ptr, 0x1234};
 *   u_sprintf_u32(uc, dst, fmt, args, 2);
 * @endcode
 *
 * 注意：u_va_start_regs() 依赖「PC 正好停在函数第一条指令」这一前提
 * （本项目 trap 机制天然满足：trap 地址即函数入口，SP 尚未被序言修改）。
 */
#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/** 客户机地址类型（ARM32） */
typedef uint32_t u_ptr;
#define U_NULL ((u_ptr)0)

/* ---- core：基建 ---- */
#include "include/u_arg.h"
#include "include/u_fmt.h"
#include "include/u_heap.h"
#include "include/u_mem.h"

/* ---- std：标准 C ---- */
#include "include/u_ctype.h"
#include "include/u_jmp.h"
#include "include/u_math.h"
#include "include/u_stdio.h"
#include "include/u_stdlib.h"
#include "include/u_str.h"
#include "include/u_time.h"
#include "include/u_wcs.h"
#include "include/u_wctype.h"

/* ---- ext：非标准扩展 ---- */
#include "include/u_fmt_ext.h"
#include "include/u_math_ext.h"
#include "include/u_mem_ext.h"
#include "include/u_str_ext.h"
#include "include/u_time_ext.h"

#endif /* ULIBC_H */

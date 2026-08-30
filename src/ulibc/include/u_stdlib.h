#ifndef U_STDLIB_H
#define U_STDLIB_H
/**
 * @file u_stdlib.h
 * @brief 进程控制、整数运算、伪随机、字符串转数值、排序 —— 标准 C（stdlib.h）
 *
 * 说明与偏离点：
 *  · abs/div         纯标量 → 自己实现（避免宿主 INT_MIN 上的 UB）
 *  · rand/srand      **不转发宿主**：模拟器需要可复现的执行轨迹，
 *                    宿主 glibc 的 rand() 序列不受控，故自建 LCG。
 *  · strtol/strtod   入参是客户机字符串 → 直接扫描客户机内存，
 *                    避免"抓到宿主缓冲再解析"的长度限制。
 *  · qsort/bsearch   标准原型要求比较器是**客户机代码地址**，需嵌套
 *                    uc_emu_start 回调，此处提供宿主侧比较器变体
 *                    （u_qsort_host / u_bsearch_host），覆盖 trap 场景。
 *  · errno           宿主的 errno 会被模拟器自身污染，故独立维护 u_errno。
 *
 * 带 _ex 后缀的取参变体（u_strtol_ex 等）是非标准便利接口：直接把结束
 * 位置返回到宿主变量，省去"先写客户机 endptr 槽再读回来"。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

/** 客户机 errno（宿主的 errno 会被模拟器自身污染，必须独立维护） */
extern int u_errno;

#define U_EDOM 33
#define U_EILSEQ 84
#define U_ERANGE 34

/* -------------------- 进程控制 -------------------- */

typedef void (*u_exit_fn)(uc_engine *uc, int code);

/** 注册 exit/abort 回调；默认行为是 uc_emu_stop */
void u_set_exit_handler(u_exit_fn fn);
void u_exit(uc_engine *uc, int code);
void u_abort(uc_engine *uc);

/* -------------------- 绝对值 / 除法 -------------------- */

int u_abs(int v);
long u_labs(long v);
long long u_llabs(long long v);

typedef struct {
  int quot;
  int rem;
} u_div_t;

typedef struct {
  long quot;
  long rem;
} u_ldiv_t;

u_div_t u_div(int num, int den);
u_ldiv_t u_ldiv(long num, long den);

/* -------------------- 伪随机（确定性 LCG） -------------------- */

void u_srand(uint32_t seed);
int u_rand(void);

/* -------------------- 字符串 → 数值 -------------------- */

int u_atoi(uc_engine *uc, uint32_t s);
long u_atol(uc_engine *uc, uint32_t s);
long long u_atoll(uc_engine *uc, uint32_t s);
double u_atof(uc_engine *uc, uint32_t s);

long u_strtol(uc_engine *uc, uint32_t s, uint32_t endptr_addr, int base);
unsigned long u_strtoul(uc_engine *uc, uint32_t s, uint32_t endptr_addr,
                        int base);
long long u_strtoll(uc_engine *uc, uint32_t s, uint32_t endptr_addr, int base);
double u_strtod(uc_engine *uc, uint32_t s, uint32_t endptr_addr);

/**
 * 非标准便利变体：结束位置直接返回到宿主变量 end_out（可为 NULL），
 * 不触碰客户机内存。
 */
long u_strtol_ex(uc_engine *uc, uint32_t s, uint32_t *end_out, int base);
unsigned long u_strtoul_ex(uc_engine *uc, uint32_t s, uint32_t *end_out,
                           int base);
long long u_strtoll_ex(uc_engine *uc, uint32_t s, uint32_t *end_out, int base);
double u_strtod_ex(uc_engine *uc, uint32_t s, uint32_t *end_out);

/* -------------------- 环境（stdlib.h） -------------------- */

/**
 * getenv：名字串在客户机内存，结果写入客户机 buf。
 * @return 实际写入字节数（含 '\0'）；变量不存在返回 0；
 *         buf 为 0 或 buflen 为 0 时只返回所需长度。
 */
uint32_t u_getenv(uc_engine *uc, uint32_t name, uint32_t buf,
                  uint32_t buflen);

/**
 * system：执行宿主 shell 命令（命令串在客户机内存）。
 * 安全提示：会真的在宿主机上执行命令，处理不可信 applet 时应禁用。
 */
int u_system(uc_engine *uc, uint32_t cmd);

/* -------------------- atexit -------------------- */

/** 注册的回调在 u_exit 时按**逆序**调用（与标准一致），上限 32 个 */
int u_atexit(void (*fn)(uc_engine *uc, void *arg), void *arg);

/* -------------------- 排序 / 查找（比较器在宿主侧） -------------------- */

typedef int (*u_compare_fn)(uc_engine *uc, uint32_t a, uint32_t b);

void u_qsort_host(uc_engine *uc, uint32_t base, uint32_t n, uint32_t width,
                  u_compare_fn cmp);

/** 命中返回客户机地址，未命中返回 0 */
uint32_t u_bsearch_host(uc_engine *uc, uint32_t key, uint32_t base,
                        uint32_t n, uint32_t width, u_compare_fn cmp);

#endif /* U_STDLIB_H */

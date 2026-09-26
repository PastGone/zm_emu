#ifndef ZM_ROOT_H
#define ZM_ROOT_H

// -------------------- ROOT_TABLE_ADDR vtable 缺失 trap 的处理函数声明
// -------------------- 00000405.app 通过 [BLOB_BASE+0x180] 取 ROOT_TABLE_ADDR
// 指针后，(*ROOT_TABLE_ADDR+offset) 调用大量 导入。本模块提供其中缺失槽位的实现：memset /
// str_assign 给真实实现， 其余为 stub（log + 返回
// 0），便于按日志频次逐个细化为真实实现。
#include <stdint.h>
#include <unicorn/unicorn.h>

/* ROOT_TABLE_ADDR[0x78]：str_assign(str_obj, cstr) —— 把 C 串赋给 zmaee 字符串对象。
 * 实现已就绪，但 TR_root_str_assign 在 emu.h 中与 TR_svc09_x2C 槽位冲突
 * （同为 ROOT_TABLE_ADDR+0x30），故暂未接线；修正槽位表后加上 case 即可启用。 */
uint32_t zm_root_str_assign(uc_engine *uc, uint32_t str_obj, uint32_t cstr_ptr);

/* ROOT_TABLE_ADDR[0x154]：返回回调对象 CBK_OBJ（vt[+8] 会被 applet 覆写为 sub_82FF8） */
uint32_t zm_root_create_cbk(uc_engine *uc);

/* ROOT_TABLE_ADDR[0xD8]：返回时间戳（SDL_GetTicks） */
uint32_t zm_root_get_tick(uc_engine *uc);

/* ROOT_TABLE_ADDR[0x68C]：zmaee 惰性资源数据访问。
 * 00000506 @0x89F50 现场：r0=0x820600（其 +0x28 处是 "data" 字符串）、
 * r1=调用方 0x40 字节缓冲、r2=0x28、r3=槽地址本身。
 * 把对象携带的数据块拷到调用方缓冲并返回对象 +0。 */
uint32_t zm_root_x68C(uc_engine *uc, uint32_t obj, uint32_t out_buf, uint32_t len, uint32_t self);

/* CBK 对象 vt[+8] 初始默认实现（会被 applet 覆写，覆写前若被调用则走此 stub）
 */
uint32_t zm_root_cbk_default(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);

/* ROOT_TABLE_ADDR[0x40] / ROOT_TABLE_ADDR[0x44]：srand / rand（00000506 实测）
 *
 * 依据：两槽相邻；0x40 全程只调 1 次且 r0=0x2D2A（种子），0x44 被大量调用。
 * 0x44 的 thunk 是 0x18E14（`ldr r0,[r0,#0x44]; bx r0`），其调用点：
 *   0x08DF4: bl 0x18E14 ; and r0, r0, #1        → 抛硬币，典型 rand()&1
 *   0x08E04: bl 0x18E14 ; lsl r4, r0, #0xf
 *   0x08E0C: bl 0x18E14 ; orr r0, r0, r4
 *   0x08E14: bic r0, r0, #0x80000000            → 两次取值拼 31 位正整数
 * 该槽缺失时恒返回 0，会让鱼群/炮弹的随机分布全部退化成固定值。
 */
void zm_root_srand(uint32_t seed);
uint32_t zm_root_rand(void);

/* ---- ROOT_TABLE_ADDR 导入表中的双精度数学函数（00000506 实测）----
 *
 * ARM EABI：double 参数放在 r0(低 32 位) / r1(高 32 位)，返回值同样占 r0:r1。
 * trap 框架只回写 r0，所以本函数自行把高 32 位写入 R1。
 *
 * 反推依据（00000506）：
 *   0x104 = sqrt —— @0xAC6C：`mul dx*dx` + `mla +dy*dy` → i2d(0x1E488) → 本槽
 *                    → d2i(0x1D6A0)，即 (int)sqrt(dx²+dy²)，求两点距离。
 *   0x114 = cos  \
 *   0x118 = sin  /  @0xACDC / @0xAD18：对**同一个**角度各调一次（实测参数
 *                   恒为双精度 2π），结果各乘上面算出的距离后加到基准
 *                   x / y 上 —— 标准极坐标转直角坐标 x=x0+r·cos(a)、
 *                   y=y0+r·sin(a)。先算 x 故 0x114=cos、0x118=sin。
 */
#define ZM_MATH_SQRT 0
#define ZM_MATH_ATAN 3 /* atan(x) */
#define ZM_MATH_TAN 4  /* tan(x) */
#define ZM_MATH_COS 1
#define ZM_MATH_SIN 2
uint32_t zm_root_math(uc_engine *uc, int op, uint32_t lo, uint32_t hi);

#endif

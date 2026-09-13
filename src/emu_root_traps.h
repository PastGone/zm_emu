#ifndef EMU_ROOT_TRAPS_H
#define EMU_ROOT_TRAPS_H

#include "emu_mem_layout.h"

/* -------------------- ROOT_TABLE_ADDR 枚举 -------------------- */
enum ZM_ROOT_TABLE {
  ZM_GetShell = 0x00U,
  ZM_Malloc = 0x08U,
  ZM_Free = 0x0cU,
  ZM_Abort = 0x14U,
  ZM_StrCopy = 0x20U,
  ZM_SRand = 0x40U,
  ZM_Rand = 0x44U,
  ZM_MemCmp = 0x50U,
  ZM_MemCpy = 0x5CU,
  ZM_Memset = 0x60U,
  ZM_Sprintf = 0x6cU,
  ZM_StrAssign = 0x78U,
  ZM_StrCtor = 0x88U,
  ZM_StrChr = 0x90U,
  ZM_SpecLookup = 0xa4U,
  ZM_StrFind = 0xa8U,
  ZM_StrStr = 0xb0U,
  ZM_GetTick = 0xD8U,
  ZM_Sqrt = 0x104U,
  ZM_Atan = 0x110U,
  ZM_Cos = 0x114U,
  ZM_Sin = 0x118U,
  ZM_Tan = 0x11CU,
  ZM_x12C = 0x12CU,
  ZM_x130 = 0x130U,
  ZM_x140 = 0x140U,
  ZM_CreateCbk = 0x154U,
  ZM_x16C = 0x16CU,
  ZM_x68C = 0x68CU,
  ZM_EnterEventLoop = 0x1180U,
  ZM_RegisterEventLoop = 0x1184U,
  ZM_InitCallback = 0x118cU
};

/* FS vtable 槽，基址是 FS_VT，不跟 ROOT_TABLE_ADDR 混在一起 */
// enum ZM_FS_VT { ZM_FS_EnumFile = 0x30U };

/* -------------------- ROOT_TABLE_ADDR trap 地址定义 -------------------- */
#define TR_root_getShell TRAP(ROOT_TABLE_ADDR + ZM_GetShell)
#define TR_root_malloc TRAP(ROOT_TABLE_ADDR + ZM_Malloc)
#define TR_root_free TRAP(ROOT_TABLE_ADDR + ZM_Free)
#define TR_root_str_copy TRAP(ROOT_TABLE_ADDR + ZM_StrCopy)
#define TR_root_sprintf TRAP(ROOT_TABLE_ADDR + ZM_Sprintf)
#define TR_root_str_ctor TRAP(ROOT_TABLE_ADDR + ZM_StrCtor)
#define TR_root_spec_lookup TRAP(ROOT_TABLE_ADDR + ZM_SpecLookup)

/* ROOT_TABLE_ADDR+0xB0 = zmaee_strstr(haystack, needle)：子串查找。
 * applet 用它判断资源名后缀（如 strstr(name, ".zbmp")），
 * 未实现会导致 .zbmp 资源被误判成 png → 走错加载分支而崩溃。 */
#define TR_root_strstr TRAP(ROOT_TABLE_ADDR + ZM_StrStr)
#define TR_root_str_find TRAP(ROOT_TABLE_ADDR + ZM_StrFind)

/*
 * 实测修正：applet 00000440 实际跳转 0x8A0060，即 ROOT_TABLE_ADDR+0x60；
 * 原先写成 ROOT_TABLE_ADDR+0x28 导致该 case 永不命中（memset 落到 default
 * 分支）。 注意这一段的偏移与注释普遍对不上，其他条目待逐个用真实 applet 验证。
 */
#define TR_root_memset TRAP(ROOT_TABLE_ADDR + ZM_Memset)

/* +0x50 memcmp（RE zmaee_memcmp @0x363E8，tramp 桩 0xb9b50）；+0x5C memcpy
 * （RE zmaee_memcpy @0x36470，桩 0xb9b40）。00001b62 调用现场+返回值用法确认。
 */
#define TR_root_memcmp TRAP(ROOT_TABLE_ADDR + ZM_MemCmp)
#define TR_root_memcpy TRAP(ROOT_TABLE_ADDR + ZM_MemCpy)

/* +0x90 strchr：调用现场 r1='r' + strb 写回，strchr 家族 */
#define TR_root_strchr TRAP(ROOT_TABLE_ADDR + ZM_StrChr)

#define TR_root_str_assign                                                     \
  TRAP(ROOT_TABLE_ADDR + ZM_StrAssign) /* str_assign(str_obj, cstr) */

#define TR_root_get_tick                                                       \
  TRAP(ROOT_TABLE_ADDR + ZM_GetTick) /* ROOT_TABLE_ADDR[0xD8] */

/* ROOT_TABLE_ADDR 导入表的双精度数学函数（00000506 实测，见 zm_root.h 注释）。
 * 这几个槽若缺失会返回 0：sin/cos 为 0 会让极坐标算出的坐标全部塌到
 * 基准点，鱼群/炮弹位置失真，实测表现为"资源加载了却没有任何绘制"。 */
#define TR_root_srand TRAP(ROOT_TABLE_ADDR + ZM_SRand) /* srand(seed) */
#define TR_root_rand TRAP(ROOT_TABLE_ADDR + ZM_Rand)   /* rand() */
#define TR_root_sqrt TRAP(ROOT_TABLE_ADDR + ZM_Sqrt)   /* (double)->double */
#define TR_root_cos TRAP(ROOT_TABLE_ADDR + ZM_Cos)     /* (double)->double */
#define TR_root_sin TRAP(ROOT_TABLE_ADDR + ZM_Sin)     /* (double)->double */

/* applet 00000506 只导入 5 个数学函数（由其导入跳板的
 * `LDR R2,[R2,#(loc_XXX - 0x140)]` 模式枚举，槽位 = XXX - 0x140）：
 *   0x104 sqrt   0x110 atan   0x114 cos   0x118 sin   0x11C tan
 * 其中 0x110/0x11C 原先未接线，被调用时报"非法的外部调用"并导致崩溃。 */
#define TR_root_atan                                                           \
  TRAP(ROOT_TABLE_ADDR + ZM_Atan) /* 实测：atan(dy/dx) 求角度 */
#define TR_root_tan                                                            \
  TRAP(ROOT_TABLE_ADDR + ZM_Tan) /* 已定案：tan（见 zm_root.c 注释） */

#define TR_root_x12C TRAP(ROOT_TABLE_ADDR + ZM_x12C)
#define TR_root_x130 TRAP(ROOT_TABLE_ADDR + ZM_x130)
#define TR_root_x140 TRAP(ROOT_TABLE_ADDR + ZM_x140)
#define TR_root_create_cbk TRAP(ROOT_TABLE_ADDR + ZM_CreateCbk)
#define TR_root_x16C TRAP(ROOT_TABLE_ADDR + ZM_x16C)

/* ROOT_TABLE_ADDR+0x68C（经 00000506 实测：r0 指向含 "data" 的对象 0x820600，
 * r1 是个 0x40 字节缓冲，r2=0x28，r3=调用槽地址本身）。
 * 语义按 zmaee 的惰性资源加载（"resourceData" 的包装对象）处理，
 * 见 zm_root_x68C：把对象数据拷进调用方缓冲，并返回对象首字段。 */
#define TR_root_x68C TRAP(ROOT_TABLE_ADDR + ZM_x68C)

/* 00000405.app：FS vtable 缺失槽（FS_VT 由 fs 模块另行定义） */
#define TR_fs_enum TRAP(FS_VT + ZM_FS_EnumFile) /* FS_VT[0x30]：enumFile */

/* 事件回调相关的 ROOT_TABLE_ADDR 槽（applet 无主循环，由宿主驱动）。 */
#define TR_init_callback                                                          \
  TRAP(                                                                           \
      ROOT_TABLE_ADDR +                                                           \
      ZM_InitCallback) /* 随便写一个位置我想也应该不影响这个叫什么来.初始化回调 \
                        */

/* 事件回调因为 apple 是没有主循环的所以要用外部来完成这个主循环 */
#define TR_enter_event_loop TRAP(ROOT_TABLE_ADDR + ZM_EnterEventLoop)

/* TR_init_callback 执行期间，applet 会调用 ROOT_TABLE_ADDR+0x1184
 * 把事件循环的入口 传出来（"注册主循环"）。模拟器据此单独调用
 * handler，而不是依赖那个 已经退化的 LR 约定——实测 00000506 的 init 顺序是
 *   ROOT_TABLE_ADDR+0x1184(handler) → ROOT_TABLE_ADDR+0x118c() → 返回
 * 所以必须真正注册，否则像 00000506 这类 applet 会在 init 返回后
 * 直接退出（PC 飞出 blob）。 */
#define TR_register_event_loop TRAP(ROOT_TABLE_ADDR + ZM_RegisterEventLoop)

/* applet 通过它向宿主请求退出（实测 00000506 参数字符串为 "aborted"）。 */
#define TR_abort TRAP(ROOT_TABLE_ADDR + ZM_Abort)

#endif /* EMU_ROOT_TRAPS_H */
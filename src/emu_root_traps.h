#ifndef EMU_ROOT_TRAPS_H
#define EMU_ROOT_TRAPS_H

#include "emu_mem_layout.h"

/* -------------------- ROOT_TABLE_ADDR 枚举 -------------------- */
enum ZM_ROOT_TABLE : uint32_t {
  ZM_GetShell = 0x00U,
  ZM_Malloc = 0x08U,
  ZM_Free = 0x0cU,
  ZM_Abort = 0x14U,
  ZM_Utf8ToUcs2 = 0x20U, /* 旧名 ZM_StrCopy：见下方 TR_root_utf8_to_ucs2 说明 */
  /* +0x30 / +0x34 = ZMAEE_MallocScreenMem / ZMAEE_FreeScreenMem。
   * RE（参考 libaee.so.c.txt:45018/45030）：两者就是 malloc/free 的别名：
   *   void *ZMAEE_MallocScreenMem(size_t a1) { return malloc(a1); }
   *   void  ZMAEE_FreeScreenMem(void *a1)    { free(a1); }
   * 实测 00000502 在 0x1461C 用 +0x30 要 0x25800（=240*320*2，整屏缓冲）
   * 时落到 default 返回 0 → applet 走错误分支、拿 NULL 对象解引用 → 崩在
   * pc=0x7C000000（地址 0 是 blob 头，被当成对象表读）。 */
  ZM_MallocScreen = 0x30U,
  ZM_FreeScreen = 0x34U,
  /* +0x24 = ZMAEE_Ucs2_2_Utf8：+0x20（Utf8_2_Ucs2）的反向转换。
   * RE（参考 libaee.so.c.txt:52664）：(ucs2_src, 源字符数, utf8_dst,
   * 目标字节容量) → 返回写入字节数。实测 00000502 的 sub_1CD78 用它把
   * 对象里的 11 字符宽串转成窄串再比字面量（r0=源 r1=0xB r2=栈 r3=0x40）。 */
  ZM_Ucs2ToUtf8 = 0x24U,
  /* +0x3C = 中性桩：目前只被 CBK 管理器 +0x30 那个"对象方法"槽借用
   * （见 trap.c 的 cbk_heap_init_once）。返回 0。 */
  ZM_x3C = 0x3CU,
  ZM_SRand = 0x40U,
  ZM_Rand = 0x44U,
  ZM_MemCmp = 0x50U,
  ZM_MemCpy = 0x5CU,
  ZM_Memset = 0x60U,
  ZM_Sprintf = 0x6cU,
  ZM_AtOf = 0x70U, /* 字符串→double（CString::ToDouble / atof），计算器解析输入用 */
  ZM_StrToNum = 0x74U,
  ZM_StrAssign = 0x78U,
  ZM_StrCtor = 0x88U,
  ZM_StrChr = 0x90U,
  ZM_SpecLookup = 0xa4U,
  ZM_StrFind = 0xa8U,
  ZM_StrStr = 0xb0U,
  ZM_WcsLen = 0xD8U, /* 旧名 ZM_GetTick：实测是宽字符串长度，见 TR_root_wcslen */
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
enum ZM_ROOT_TRAPS : uint32_t {
  TR_root_getShell = TRAP(ROOT_TABLE_ADDR + ZM_GetShell),
  TR_root_malloc = TRAP(ROOT_TABLE_ADDR + ZM_Malloc),
  TR_root_free = TRAP(ROOT_TABLE_ADDR + ZM_Free),
  TR_root_malloc_screen = TRAP(ROOT_TABLE_ADDR + ZM_MallocScreen),
  TR_root_free_screen = TRAP(ROOT_TABLE_ADDR + ZM_FreeScreen),
  TR_root_ucs2_to_utf8 = TRAP(ROOT_TABLE_ADDR + ZM_Ucs2ToUtf8),
  TR_root_x3C = TRAP(ROOT_TABLE_ADDR + ZM_x3C),
  /* ROOT_TABLE_ADDR+0x20 = **ZMAEE_Utf8_2_Ucs2**（UTF-8 窄串 → UCS-2 转换拷贝）
   *
   * 真机：(a1=utf8 源, a2=源**字节数**, a3=UCS-2 目标, a4=目标**字符容量**)，
   *       返回 R0 = 写入字符数（R1 另回字节数 2*count）。
   * 坐实它的现场是 00000102（数字键盘）：`sprintf("%u")` 出窄串（该 applet 的
   * 字面量 "%u"/"zmr" 全是 ASCII）→ 本槽转换 → 直接把**返回的字符数**当
   * IDisplay::DrawText 的 len 用，而 DrawText 内部只吃 UCS-2。
   *
   * 【正名】此槽以前叫 `str_copy`，被当成"按长度 memcpy、src 在前、返回字节数"。
   * 那个误判在 ASCII 样本上"看着能用"，但语义是错的：真机是把每个字符**加宽**
   * 成 16 位再写（逐个解码 1/2/3 字节 UTF-8）。实现见 zm_str.c 的 zm_utf8_to_ucs2。 */
  TR_root_utf8_to_ucs2 = TRAP(ROOT_TABLE_ADDR + ZM_Utf8ToUcs2),
  TR_root_sprintf = TRAP(ROOT_TABLE_ADDR + ZM_Sprintf),

  /* ROOT_TABLE_ADDR+0x74 = **数值字符串解析**（strtol 家族：str, endptr, base）。
   *
   * RE 证据（00000506 导入跳板 sub_18F98：`LDR R3,[R3,#(off_1B4 - 0x140)]`
   * → 槽 = 0x1B4-0x140 = 0x74，共 5 个调用点）：
   *   帮助页的标记解析器把 "{c #FF0000}" 拆成三段两字符，各调一次：
   *     R5 = f(buf,"FF",16); R6 = f(buf,"00",16); R4 = f(buf,"00",16);
   *     颜色 = R5<<16 | R6<<8 | R4 | (第 4 段 << 24)
   *   调用形状恒为 (str=r0, endptr=0, base=0x10)，返回值就是解析出的数值。
   * 这个槽以前没接线（走 default 分支返回 0），实测**每轮 3716 次**
   * "非法的外部调用"，颜色全部塌成黑。 */
  TR_root_str_to_num = TRAP(ROOT_TABLE_ADDR + ZM_StrToNum),
  TR_root_atof = TRAP(ROOT_TABLE_ADDR + ZM_AtOf),
  TR_root_str_ctor = TRAP(ROOT_TABLE_ADDR + ZM_StrCtor),
  TR_root_spec_lookup = TRAP(ROOT_TABLE_ADDR + ZM_SpecLookup),

  /* ROOT_TABLE_ADDR+0xB0 = zmaee_strstr(haystack, needle)：子串查找。
   * applet 用它判断资源名后缀（如 strstr(name, ".zbmp")），
   * 未实现会导致 .zbmp 资源被误判成 png → 走错加载分支而崩溃。 */
  TR_root_strstr = TRAP(ROOT_TABLE_ADDR + ZM_StrStr),
  TR_root_str_find = TRAP(ROOT_TABLE_ADDR + ZM_StrFind),

  /*
   * 实测修正：applet 00000440 实际跳转 0x8A0060，即 ROOT_TABLE_ADDR+0x60；
   * 原先写成 ROOT_TABLE_ADDR+0x28 导致该 case 永不命中（memset 落到 default
   * 分支）。 注意这一段的偏移与注释普遍对不上，其他条目待逐个用真实 applet 验证。
   */
  TR_root_memset = TRAP(ROOT_TABLE_ADDR + ZM_Memset),

  /* +0x50 memcmp（RE zmaee_memcmp @0x363E8，tramp 桩 0xb9b50）；+0x5C memcpy
   * （RE zmaee_memcpy @0x36470，桩 0xb9b40）。00001b62 调用现场+返回值用法确认。
   */
  TR_root_memcmp = TRAP(ROOT_TABLE_ADDR + ZM_MemCmp),
  TR_root_memcpy = TRAP(ROOT_TABLE_ADDR + ZM_MemCpy),

  /* +0x90 strchr：调用现场 r1='r' + strb 写回，strchr 家族 */
  TR_root_strchr = TRAP(ROOT_TABLE_ADDR + ZM_StrChr),

  TR_root_str_assign = TRAP(ROOT_TABLE_ADDR + ZM_StrAssign), /* str_assign(str_obj, cstr) */

  /* ROOT_TABLE_ADDR[0xD8] = **zmaee_wcslen**（宽字符串长度，返回字符数）
   *
   * 【正名】此前记成 GetTickCount（返回 SDL_GetTicks），是猜测。实测 5 处调用点
   * 全是"取长度"：506/440 把返回值当 DrawText 的 len；0000050b 与 00000001 都做
   * `LSL#1`（字符数 ×2 → 字节数）——这条是决定性证据。详见 zm_str.c 的 zm_wcslen。
   * 固件的宽字符家族（zmaee_wcscat/wcscmp/wcscpy/wcslen/wcsstr…）与窄家族并存，
   * 本槽属于宽家族。 */
  TR_root_wcslen = TRAP(ROOT_TABLE_ADDR + ZM_WcsLen),

  /* ROOT_TABLE_ADDR 导入表的双精度数学函数（00000506 实测，见 zm_root.h 注释）。
   * 这几个槽若缺失会返回 0：sin/cos 为 0 会让极坐标算出的坐标全部塌到
   * 基准点，鱼群/炮弹位置失真，实测表现为"资源加载了却没有任何绘制"。 */
  TR_root_srand = TRAP(ROOT_TABLE_ADDR + ZM_SRand), /* srand(seed) */
  TR_root_rand = TRAP(ROOT_TABLE_ADDR + ZM_Rand),   /* rand() */
  TR_root_sqrt = TRAP(ROOT_TABLE_ADDR + ZM_Sqrt),   /* (double)->double */
  TR_root_cos = TRAP(ROOT_TABLE_ADDR + ZM_Cos),     /* (double)->double */
  TR_root_sin = TRAP(ROOT_TABLE_ADDR + ZM_Sin),     /* (double)->double */

  /* applet 00000506 只导入 5 个数学函数（由其导入跳板的
   * `LDR R2,[R2,#(loc_XXX - 0x140)]` 模式枚举，槽位 = XXX - 0x140）：
   *   0x104 sqrt   0x110 atan   0x114 cos   0x118 sin   0x11C tan
   * 其中 0x110/0x11C 原先未接线，被调用时报"非法的外部调用"并导致崩溃。 */
  TR_root_atan = TRAP(ROOT_TABLE_ADDR + ZM_Atan), /* 实测：atan(dy/dx) 求角度 */
  TR_root_tan = TRAP(ROOT_TABLE_ADDR + ZM_Tan),   /* 已定案：tan（见 zm_root.c 注释） */

  TR_root_x12C = TRAP(ROOT_TABLE_ADDR + ZM_x12C),
  TR_root_x130 = TRAP(ROOT_TABLE_ADDR + ZM_x130),
  TR_root_x140 = TRAP(ROOT_TABLE_ADDR + ZM_x140),
  TR_root_create_cbk = TRAP(ROOT_TABLE_ADDR + ZM_CreateCbk),
  TR_root_x16C = TRAP(ROOT_TABLE_ADDR + ZM_x16C),

  /* ROOT_TABLE_ADDR+0x68C（经 00000506 实测：r0 指向含 "data" 的对象 0x820600，
   * r1 是个 0x40 字节缓冲，r2=0x28，r3=调用槽地址本身）。
   * 语义按 zmaee 的惰性资源加载（"resourceData" 的包装对象）处理，
   * 见 zm_root_x68C：把对象数据拷进调用方缓冲，并返回对象首字段。 */
  TR_root_x68C = TRAP(ROOT_TABLE_ADDR + ZM_x68C),

  /* 事件回调相关的 ROOT_TABLE_ADDR 槽（applet 无主循环，由宿主驱动）。 */
  /* 初始化回调 */
  TR_init_callback = TRAP(ROOT_TABLE_ADDR + ZM_InitCallback),

  /* 事件回调因为 apple 是没有主循环的所以要用外部来完成这个主循环 */
  TR_enter_event_loop = TRAP(ROOT_TABLE_ADDR + ZM_EnterEventLoop),

  /* TR_init_callback 执行期间，applet 会调用 ROOT_TABLE_ADDR+0x1184
   * 把事件循环的入口 传出来（"注册主循环"）。模拟器据此单独调用
   * handler，而不是依赖那个 已经退化的 LR 约定——实测 00000506 的 init 顺序是
   *   ROOT_TABLE_ADDR+0x1184(handler) → ROOT_TABLE_ADDR+0x118c() → 返回
   * 所以必须真正注册，否则像 00000506 这类 applet 会在 init 返回后
   * 直接退出（PC 飞出 blob）。 */
  TR_register_event_loop = TRAP(ROOT_TABLE_ADDR + ZM_RegisterEventLoop),

  /* applet 通过它向宿主请求退出（实测 00000506 参数字符串为 "aborted"）。 */
  TR_abort = TRAP(ROOT_TABLE_ADDR + ZM_Abort)
};

/* 00000405.app：FS vtable 缺失槽（FS_VT 由 fs 模块另行定义）。
 * 其基址 FS_VT / 枚举 ZM_FS_EnumFile 当前由 fs 模块另行提供、尚不在本头可见，
 * 故保留为宏：宏仅在使用处做文本展开、未使用不会触发"未定义标识符"错误，
 * 待 RE 与 fs 模块接入后随其余槽一并并入 ZM_ROOT_TRAPS 枚举。 */
#define TR_fs_enum TRAP(FS_VT + ZM_FS_EnumFile) /* FS_VT[0x30]：enumFile */

#endif /* EMU_ROOT_TRAPS_H */

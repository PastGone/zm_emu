#include "zm_root.h"

#include <SDL2/SDL.h>  /* SDL_GetTicks  (ROOT_TABLE_ADDR[0xD8] get_tick) */
#include <math.h>      /* sqrt/sin/cos  (ROOT_TABLE_ADDR 导入表的双精度数学函数) */
#include <stdlib.h>    /* srand/rand   (ROOT_TABLE_ADDR[0x40]/[0x44]) */
#include <string.h>    /* strlen        (str_assign) */

#include "../../emu.h" /* CBK_OBJ / SHIM_BASE 等地址常量 */
#include "../../log/log.h"
#include "../../tool/uc_helper.h"
#include "zm_str.h" /* read_cstr */

/*
 * zm_root_memset 已删除：TR_root_memset 现接到 ulibc 的 u_memset
 * （见 trap.c），本函数成为死代码。
 */

/**
 * @brief ROOT[0x78] str_assign(str_obj, cstr_ptr)
 *
 * 把 C 字符串赋值给 zmaee 字符串对象：
 *   +0  : 数据指针（指向 +12 内联缓冲）
 *   +4  : 长度
 *   +8  : 容量
 *   +12 : 内联字符串数据（含 '\0'）
 * sub_841D4 中：ADR R1,"app_list"; BL sub_84868。
 */
uint32_t zm_root_str_assign(uc_engine *uc, uint32_t str_obj,
                            uint32_t cstr_ptr) {
  if (str_obj == 0)
    return str_obj;
  char cstr[256];
  read_cstr(uc, cstr_ptr, cstr, sizeof(cstr));
  uint32_t clen = (uint32_t)strlen(cstr);
  if (clen > 255)
    clen = 255;
  uint32_t inline_buf = str_obj + 12;

  /* 【两种目标形态，实测都存在】上面那套"对象布局"只是其中一种：
   *   (a) zmaee 字符串对象：+0=data_ptr、+4=len、+8=cap、+12=内联数据；
   *   (b) **裸 C 串缓冲**：applet 先 `memset(buf, 0, 0x20)` 再调本函数当 strcpy 用。
   *
   * 判据：缓冲里已经有"像 data_ptr/len 的对象头"才走 (a)，否则按裸串写。
   *   - data_ptr == str_obj + 12 （内联布局，我们上次写的就是这个）→ 对象；
   *   - data_ptr 可读且 len < 4096 → 对象（堆/栈上的对象）；
   *   - 其余（首字为 0、或是一串文本、或是野值）→ 裸串。
   *
   * 为什么必须判（0000042f《落井下石》实测 崩溃根因）：它把模板串
   * "e:\zmol\zmdata\qblox.dat" 拼进栈上 32 字节缓冲（先 memset、再 strb 盘符、
   * 再调本函数）后交给 FileMgr::Open。以前无条件按 (a) 写 → 缓冲首字被写成
   * **指针**，文件名于是变成指针的字节（"E\xFB\x17" = 0x17FB45 的小端），
   * 路径全错；紧接着 applet 把这段字符串当对象用，崩在 0x66E8
   * （`ldrne r1,[r0,#0x10]`，r0=0x7461642E=".dat"）。 */
  {
    uint32_t w0 = 0, w1 = 0;
    bool as_obj = false;
    if (uc_mem_read(uc, str_obj, &w0, 4) == UC_ERR_OK &&
        uc_mem_read(uc, str_obj + 4, &w1, 4) == UC_ERR_OK) {
      if (w0 == inline_buf) {
        as_obj = true;
      } else if (w0 != 0 && w1 < 4096) {
        uint8_t probe = 0;
        if (uc_mem_read(uc, w0, &probe, 1) == UC_ERR_OK)
          as_obj = true;
      }
    }
    if (!as_obj) {
      /* 裸 C 串缓冲：直接写串（含收尾 '\0'），不做任何对象字段包装 */
      if (uc_mem_write(uc, str_obj, cstr, clen + 1) != UC_ERR_OK)
        log_warn("str_assign: 裸串写入 0x%X 失败（%u 字节）", str_obj, clen + 1);
      return str_obj;
    }
  }

  uc_write32(uc, str_obj, inline_buf); /* 数据指针 */
  uc_write32(uc, str_obj + 4, clen);   /* 长度 */
  uc_write32(uc, str_obj + 8, clen);   /* 容量 */
  uc_mem_write(uc, inline_buf, cstr, clen + 1);
  return str_obj;
}

/* ROOT_TABLE_ADDR[0x154]：返回 CBK_OBJ。applet 随后会覆写 CBK_OBJ_VT_ADDR[+8] 为 sub_82FF8。 */
uint32_t zm_root_create_cbk(uc_engine *uc) {
  (void)uc;
  return CBK_OBJ;
}

/* ROOT_TABLE_ADDR[0x40]/[0x44]：srand / rand。种子只播一次，之后交给 libc。 */
void zm_root_srand(uint32_t seed) {
  static bool seeded = false;
  if (!seeded) {
    srand(seed);
    seeded = true;
  }
}

uint32_t zm_root_rand(void) { return (uint32_t)rand(); }

/* 把 double 结果按 EABI 拆成 r0(低) / r1(高)。r1 需自己写寄存器：
 * trap 框架只回写 R0。 */
static uint32_t ret_double(uc_engine *uc, double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  uint32_t hi = (uint32_t)(bits >> 32);
  uc_reg_write(uc, UC_ARM_REG_R1, &hi);
  return (uint32_t)(bits & 0xFFFFFFFFu);
}

uint32_t zm_root_math(uc_engine *uc, int op, uint32_t lo, uint32_t hi) {
  uint64_t bits = ((uint64_t)hi << 32) | (uint64_t)lo;
  double x;
  memcpy(&x, &bits, sizeof(x));

  switch (op) {
  case ZM_MATH_SQRT:
    return ret_double(uc, sqrt(x));
  case ZM_MATH_COS:
    return ret_double(uc, cos(x));
  case ZM_MATH_SIN:
    return ret_double(uc, sin(x));
  case ZM_MATH_ATAN:
    /* ROOT_TABLE_ADDR[0x110]。依据 applet 调用点（lr=0x17558）：
     *   BL sub_1DE28        （dy/dx 相除）
     *   BL sub_19290        ← 本函数（一元 double）
     *   LDM R11,{R2,R3} + BL sub_1E538   （乘 180/π）
     *   SUB R0, R0, #0x5A   （减 90）
     * 是标准的 atan 求角度写法。 */
    return ret_double(uc, atan(x));
  case ZM_MATH_TAN:
    /* ROOT_TABLE_ADDR[0x11C]。定案依据：applet 只导入 5 个数学函数（由其导入跳板的
     * `LDR R2,[R2,#(loc_XXX - 0x140)]` 模式枚举出槽位），其中
     * 0x104=sqrt / 0x110=atan / 0x114=cos / 0x118=sin 四个槽的实测值把
     * 位置钉死了，剩下的只能是 docs/可能有的函数.md 里
     * 「cos、sin、tan」相邻顺序的第 5 个 = tan。
     * 实测实参 0.7679 / 3.2638 / 2.6005（≈44°/187°/149°）也符合角度分布。 */
    return ret_double(uc, tan(x));
  default:
    return ret_double(uc, 0.0);
  }
}

/* ROOT_TABLE_ADDR[0xD8]：返回 SDL_GetTicks() 时间戳 */
uint32_t zm_root_x68C(uc_engine *uc, uint32_t obj, uint32_t out_buf,
                      uint32_t len, uint32_t self) {
  (void)self;
  if (obj == 0)
    return 0;

  uint32_t head = 0;
  if (uc_mem_read(uc, obj, &head, 4) != UC_ERR_OK)
    return 0;

  /* 把对象的载荷（+0x14 起，若可读）拷进调用方缓冲。
   * 真机这里做的是"惰性加载资源数据"，模拟器没有对应的数据源，
   * 保持缓冲原样、只回填头字段即可保证调用方不读飞。 */
  if (out_buf && len) {
    uint32_t src = obj + 0x14;
    uint8_t probe = 0;
    if (uc_mem_read(uc, src, &probe, 1) == UC_ERR_OK) {
      uint32_t n = len > 0x40 ? 0x40 : len;
      uint8_t tmp[0x40];
      if (uc_mem_read(uc, src, tmp, n) == UC_ERR_OK)
        uc_mem_write(uc, out_buf, tmp, n);
    }
  }
  log_debug("root[0x68C]: obj=0x%X head=%u out=0x%X len=%u", obj, head, out_buf,
            len);
  return head;
}

uint32_t zm_root_get_tick(uc_engine *uc) {
  (void)uc;
  return (uint32_t)SDL_GetTicks();
}

/* zm_root_stub 已删除：零引用，未接线槽位现由 trap.c 的 default 分支
 * 直接 log_error("非法的外部调用") 处理，比通用 stub 定位更精确。 */

/* CBK 对象 vt[+8] 默认实现（覆写前若被调用） */
uint32_t zm_root_cbk_default(uc_engine *uc, uint32_t r0, uint32_t r1,
                             uint32_t r2, uint32_t r3) {
  (void)uc;
  log_info("stub cbk_default r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}


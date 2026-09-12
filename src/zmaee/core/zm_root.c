#include "zm_root.h"

#include <SDL2/SDL.h>  /* SDL_GetTicks  (ROOT[0xD8] get_tick) */
#include <math.h>      /* sqrt/sin/cos  (ROOT 导入表的双精度数学函数) */
#include <stdlib.h>    /* srand/rand   (ROOT[0x40]/[0x44]) */
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
  uc_write32(uc, str_obj, inline_buf); /* 数据指针 */
  uc_write32(uc, str_obj + 4, clen);   /* 长度 */
  uc_write32(uc, str_obj + 8, clen);   /* 容量 */
  uc_mem_write(uc, inline_buf, cstr, clen + 1);
  return str_obj;
}

/* ROOT[0x154]：返回 CBK_OBJ。applet 随后会覆写 CBK_OBJ_VT[+8] 为 sub_82FF8。 */
uint32_t zm_root_create_cbk(uc_engine *uc) {
  (void)uc;
  return CBK_OBJ;
}

/* ROOT[0x40]/[0x44]：srand / rand。种子只播一次，之后交给 libc。 */
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
  default:
    return ret_double(uc, 0.0);
  }
}

/* ROOT[0xD8]：返回 SDL_GetTicks() 时间戳 */
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

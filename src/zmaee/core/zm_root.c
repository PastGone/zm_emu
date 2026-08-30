#include "zm_root.h"

#include <SDL2/SDL.h>  /* SDL_GetTicks  (ROOT[0xD8] get_tick) */
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

/* ROOT[0xD8]：返回 SDL_GetTicks() 时间戳 */
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

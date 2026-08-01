#include "zm_root.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "../../log/log.h"
#include "../../tool/uc_helper.h"
#include "zm_addrs.h"
#include "zm_str.h" /* read_cstr */

/**
 * @brief ROOT[0x60] memset(dst, val, len)
 *
 * sub_841D4 中：MOV R2,#0x214; MOV R1,#0; BL sub_8475C
 * 即 memset(v2, 0, 0x214)。val 取低 8 位按字节填充。
 */
uint32_t zm_root_memset(uc_engine *uc, uint32_t dst, uint32_t val,
                        uint32_t len) {
  if (len == 0 || dst == 0)
    return dst;
  uint8_t fill = (uint8_t)(val & 0xFF);
  /* 按块分配填充，避免大 len 时栈溢出 */
  uint32_t chunk = 4096;
  uint8_t *buf = malloc(chunk);
  if (!buf)
    return dst;
  memset(buf, fill, chunk > len ? len : chunk);
  uint32_t off = 0;
  while (off < len) {
    uint32_t n = (len - off < chunk) ? (len - off) : chunk;
    uc_mem_write(uc, dst + off, buf, n);
    off += n;
  }
  free(buf);
  return dst;
}

/**
 * @brief ROOT[0x78] str_assign(str_obj, cstr_ptr)
 *
 * 把 C 字符串赋值给 zmaee 字符串对象（与 zm_strcpy_cstr
 * 写回客户机的布局相同）： +0  : 数据指针（指向 +12 内联缓冲） +4  : 长度 +8  :
 * 容量 +12 : 内联字符串数据（含 '\0'） sub_841D4 中：ADR R1,"app_list"; BL
 * sub_84868。
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
// uint32_t zm_root_create_cbk(uc_engine *uc) {
//   (void)uc;
//   return CBK_OBJ;
// }

/* ROOT[0xD8]：返回 SDL_GetTicks() 时间戳 */
uint32_t zm_root_get_tick(uc_engine *uc) {
  (void)uc;
  return (uint32_t)SDL_GetTicks();
}

/* 通用 stub：记录命中的 ROOT vtable 偏移与参数，返回 0 */
uint32_t zm_root_stub(uc_engine *uc, uint32_t offset, uint32_t r0, uint32_t r1,
                      uint32_t r2, uint32_t r3) {
  (void)uc;
  log_info("stub root[0x%X] r0=%u r1=%u r2=%u r3=%u", offset, r0, r1, r2, r3);
  return 0;
}

/* CBK 对象 vt[+8] 默认实现（覆写前若被调用） */
uint32_t zm_root_cbk_default(uc_engine *uc, uint32_t r0, uint32_t r1,
                             uint32_t r2, uint32_t r3) {
  (void)uc;
  log_info("stub cbk_default r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

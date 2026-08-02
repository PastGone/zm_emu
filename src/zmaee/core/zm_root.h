#ifndef ZM_ROOT_H
#define ZM_ROOT_H

// -------------------- ROOT vtable 缺失 trap 的处理函数声明
// -------------------- 00000405.app 通过 [BLOB_BASE+0x180] 取 ROOT
// 指针后，(*ROOT+offset) 调用大量 导入。本模块提供其中缺失槽位的实现：memset /
// str_assign 给真实实现， 其余为 stub（log + 返回
// 0），便于按日志频次逐个细化为真实实现。
#include <stdint.h>
#include <unicorn/unicorn.h>

/* ROOT[0x60]：memset(dst, val, len) —— sub_841D4 用 MOV R2,#0x214;MOV R1,#0
 * 调用 */
uint32_t zm_root_memset(uc_engine *uc, uint32_t dst, uint32_t val,
                        uint32_t len);

/* ROOT[0x78]：str_assign(str_obj, cstr) —— 把 C 串赋给 zmaee 字符串对象 */
uint32_t zm_root_str_assign(uc_engine *uc, uint32_t str_obj, uint32_t cstr_ptr);

/* ROOT[0x154]：返回回调对象 CBK_OBJ（vt[+8] 会被 applet 覆写为 sub_82FF8） */
uint32_t zm_root_create_cbk(uc_engine *uc);

/* ROOT[0xD8]：返回时间戳（SDL_GetTicks） */
uint32_t zm_root_get_tick(uc_engine *uc);

/* 通用 stub：记录命中的 vtable 偏移与参数后返回 0。
 * offset 为 ROOT vtable 偏移（仅用于日志标识）。 */
uint32_t zm_root_stub(uc_engine *uc, uint32_t offset, uint32_t r0, uint32_t r1,
                      uint32_t r2, uint32_t r3);

/* CBK 对象 vt[+8] 初始默认实现（会被 applet 覆写，覆写前若被调用则走此 stub）
 */
uint32_t zm_root_cbk_default(uc_engine *uc, uint32_t r0, uint32_t r1,
                             uint32_t r2, uint32_t r3);

#endif

#ifndef ZM_ROOT_H
#define ZM_ROOT_H

// -------------------- ROOT vtable 缺失 trap 的处理函数声明
// -------------------- 00000405.app 通过 [BLOB_BASE+0x180] 取 ROOT
// 指针后，(*ROOT+offset) 调用大量 导入。本模块提供其中缺失槽位的实现：memset /
// str_assign 给真实实现， 其余为 stub（log + 返回
// 0），便于按日志频次逐个细化为真实实现。
#include <stdint.h>
#include <unicorn/unicorn.h>

/* ROOT[0x78]：str_assign(str_obj, cstr) —— 把 C 串赋给 zmaee 字符串对象。
 * 实现已就绪，但 TR_root_str_assign 在 emu.h 中与 TR_svc09_x2C 槽位冲突
 * （同为 ROOT+0x30），故暂未接线；修正槽位表后加上 case 即可启用。 */
uint32_t zm_root_str_assign(uc_engine *uc, uint32_t str_obj, uint32_t cstr_ptr);

/* ROOT[0x154]：返回回调对象 CBK_OBJ（vt[+8] 会被 applet 覆写为 sub_82FF8） */
uint32_t zm_root_create_cbk(uc_engine *uc);

/* ROOT[0xD8]：返回时间戳（SDL_GetTicks） */
uint32_t zm_root_get_tick(uc_engine *uc);

/* CBK 对象 vt[+8] 初始默认实现（会被 applet 覆写，覆写前若被调用则走此 stub）
 */
uint32_t zm_root_cbk_default(uc_engine *uc, uint32_t r0, uint32_t r1,
                             uint32_t r2, uint32_t r3);

#endif

#ifndef ZM_ROOT_H
#define ZM_ROOT_H

// -------------------- ROOT vtable 缺失 trap 的处理函数声明
// -------------------- 00000405.app 通过 [BLOB_BASE+0x180] 取 ROOT
// 指针后，(*ROOT+offset) 调用大量 导入。本模块提供其中缺失槽位的实现：memset /
// str_assign 给真实实现， 其余为 stub（log + 返回
// 0），便于按日志频次逐个细化为真实实现。
#include <stdint.h>
#include <unicorn/unicorn.h>

/* ---- ROOT 表里的标准 C 库槽位（偏移由 0000050c 的 thunk 表反推） ---- */
uint32_t zm_root_memcmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n);
uint32_t zm_root_memmove(uc_engine *uc, uint32_t dst, uint32_t src,
                         uint32_t len);
uint32_t zm_root_memcpy(uc_engine *uc, uint32_t dst, uint32_t src,
                        uint32_t len);
uint32_t zm_root_strlen(uc_engine *uc, uint32_t s);
uint32_t zm_root_strstr(uc_engine *uc, uint32_t hay_ptr, uint32_t needle_ptr);
uint32_t zm_root_strtol(uc_engine *uc, uint32_t nptr, uint32_t endptr,
                        uint32_t base);
uint32_t zm_root_wcstombs(uc_engine *uc, uint32_t src, uint32_t len,
                          uint32_t dst, uint32_t cap);
uint32_t zm_root_strtod(uc_engine *uc, uint32_t nptr, uint32_t endptr);

/* ROOT[0x60]：memset(dst, val, len) —— sub_841D4 用 MOV R2,#0x214;MOV R1,#0
 * 调用 */
uint32_t zm_root_memset(uc_engine *uc, uint32_t dst, uint32_t val,
                        uint32_t len);

/* ROOT[0x78]：str_assign(str_obj, cstr) —— 把 C 串赋给 zmaee 字符串对象 */
uint32_t zm_root_str_assign(uc_engine *uc, uint32_t str_obj, uint32_t cstr_ptr);

/* ROOT[0x4]：取上下文对象（00000440），返回可偏移访问的缓冲基址 */
uint32_t zm_root_get_ctx(uc_engine *uc, uint32_t sp_buf, uint32_t flag);

/* ROOT[0x3C]：调试输出（0000050b），no-op 返 0 */
uint32_t zm_root_trace(uc_engine *uc, uint32_t fmt, uint32_t text);

/* ROOT[0x144]：状态查询（0000050c），no-op 返 0 */
uint32_t zm_root_get_status(uc_engine *uc, uint32_t a, uint32_t b);

/* ROOT[0x140]：复制 UTF-16 串（00000405 sub_84C6C 填充标签） */
uint32_t zm_root_str_utf16_copy(uc_engine *uc, uint32_t dest, uint32_t count,
                                uint32_t maxlen, uint32_t src);

/* ROOT[0x80]：型号查询（00000405），写默认型号串到 out_buf、返 0=默认布局 */
uint32_t zm_root_model_check(uc_engine *uc, uint32_t out_buf,
                             uint32_t model_str);

/* ROOT[0xC8]/[0xD0]：系统属性查询（00000405），写空串、返 0 */
uint32_t zm_root_prop_get(uc_engine *uc, uint32_t buf, uint32_t key);

/* ROOT[0x154]：返回回调对象 CBK_OBJ（vt[+8] 会被 applet 覆写为 sub_82FF8） */
uint32_t zm_root_create_cbk(uc_engine *uc);

/* 把 g_app_pathname（正斜杠相对路径，如 "applet/00000440/00000440.app"）
 * 转成 applet 期望的"带反斜杠完整路径"（如 "applet\\00000440\\00000440.app"）。
 * 00000440 等 applet 用 instance+4 里的反斜杠扫描目录来定位资源文件。 */
const char *zm_app_path_backslash(void);

/* ROOT[0xD8]：返回时间戳（SDL_GetTicks） */
uint32_t zm_root_get_tick(uc_engine *uc);

/* ROOT[0x148]：IShell_SetTimer(ms, callback, param) → 非零 timer_id。
 * 模拟器只记录回调地址，到期后由事件循环派发 EV_TIMER。 */
uint32_t zm_root_set_timer(uc_engine *uc, uint32_t ms, uint32_t callback,
                           uint32_t param);

/* RT_VT[0x3C]：周期定时器注册（带回调上下文 ctx，派发时写 R1）。
 * 仅 00000440 的 sub_30884 需要（ctx 取注册现场 R4），其他传 0。 */
uint32_t zm_root_set_timer_ctx(uc_engine *uc, uint32_t ms, uint32_t callback,
                               uint32_t param, uint32_t ctx);

/* ROOT[0x14C]：IShell_CancelTimer(callback) → 1/0 */
uint32_t zm_root_cancel_timer(uc_engine *uc, uint32_t callback);

/* 检查到期定时器：若有到期项，把回调地址写入 *cb_out、参数写入 *param_out、
 * 回调上下文写入 *ctx_out（可 NULL），标记该项已触发（一次性）并返回 true。 */
bool zm_root_timer_poll(uc_engine *uc, uint32_t *cb_out, uint32_t *param_out,
                        uint32_t *ctx_out);
/* 无头模式虚拟时钟：每轮事件循环调用 zm_root_timer_tick() 推进时间 */
void zm_root_timer_tick(void);
bool zm_root_timer_poll_virtual(uc_engine *uc, uint32_t *cb_out,
                                uint32_t *param_out, uint32_t *ctx_out);
bool zm_root_timer_has_active(void);

/* 通用 stub：记录命中的 vtable 偏移与参数后返回 0。
 * offset 为 ROOT vtable 偏移（仅用于日志标识）。 */
uint32_t zm_root_stub(uc_engine *uc, uint32_t offset, uint32_t r0, uint32_t r1,
                      uint32_t r2, uint32_t r3);

/* CBK 对象 vt[+8] 初始默认实现（会被 applet 覆写，覆写前若被调用则走此 stub）
 */
uint32_t zm_root_cbk_default(uc_engine *uc, uint32_t r0, uint32_t r1,
                             uint32_t r2, uint32_t r3);

#endif

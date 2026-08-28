#ifndef ZM_RUNTIME_H
#define ZM_RUNTIME_H

// -------------------- runtime 相关 trap 处理函数声明 --------------------
#include <stdint.h>
#include <unicorn/unicorn.h>

/**
 * @brief rt.queryInterface：按服务号返回对应子系统对象地址
 * @param svc     服务号（对应 r1），如 0x1000005=GFX / 0x1000003=FS /
 *                0x100000B=AUDIO / 0x100000C=AP
 * @param out_ptr 输出槽地址（对应 r2），非 0 时把对象地址写入此处
 * @return 固定返回 0
 */
uint32_t zm_rt_queryInterface(uc_engine *uc, uint32_t svc, uint32_t out_ptr);

/**
 * @brief rt.getSystemInfo：写入系统信息（屏幕宽高等）到 out_ptr
 *
 * 写入布局：+0=?, +4=?, +8=ScreenW, +12=ScreenH
 * ScreenW/ScreenH 由 zm_rt_set_screen_size 设置（取自 AppHeader），
 * 默认 240x240。
 *
 * @param out_ptr 输出地址（对应 r1）
 * @return 固定返回 0
 */
uint32_t zm_rt_getSystemInfo(uc_engine *uc, uint32_t out_ptr);

/**
 * @brief RT_VT[+0x58] loadDLL(name_ptr, name_len, out_ptr)
 *
 * sub_85248 调 (*SHELL+88)(SHELL, "zmsys001.dll", 28, &out)。
 * 本实现为 stub：读取 dll 名仅作日志，把 DLL_OBJ 写入 *out_ptr 并返回，
 * 让 applet 后续对 DLL 对象 vt[+8/+0xC/+0x10] 的调用不崩溃。
 */
uint32_t zm_rt_loadDLL(uc_engine *uc, uint32_t name_ptr, uint32_t name_len,
                       uint32_t out_ptr);

/**
 * @brief RT_VT[+0x5C] unloadDLL(handle)
 * sub_85340 调 (*RUNTIME+92)(RUNTIME, handle)。stub：log + 返 0。
 */
uint32_t zm_rt_unloadDLL(uc_engine *uc, uint32_t handle);

/**
 * @brief RT_VT[+0x78] loadDLL2(this, buf, size, out_obj_ptr, alloc_buf,
 * alloc_sz)
 *
 * sub_83E24 调 (*SHELL+120)(SHELL, buf, 20, &v2[1], a1[116], a1[117])。
 * 语义：按名载入模块（zmsys006.dll），把模块对象指针写入 *out_obj_ptr，
 * 返回非 0 表成功。applet 随后检查返回值与 *out_obj_ptr 均非 0 才继续，
 * 再调 (*out_obj_ptr)->vt[0x0C](...)。
 *
 * stub：把 DLL_OBJ 写入 *r3 并返回 1，使 applet 走 DLL_OBJ->vt[0x0C]
 * （已 stub 为 zm_dll_config）。
 */
uint32_t zm_rt_loadDLL2(uc_engine *uc, uint32_t r0, uint32_t buf, uint32_t size,
                        uint32_t out_obj_ptr);

/* 服务对象 release（SVC04_VT[+4] / SVC09_VT[+4]）：无操作返 0 */
uint32_t zm_svc_release(uc_engine *uc);

/* SVC04_VT[+0x1C]：stub */
uint32_t zm_svc04_x1C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                      uint32_t r3);

/* SVC09_VT[+0x2C] / [+0x40]：stub */
uint32_t zm_svc09_x2C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                      uint32_t r3);
uint32_t zm_svc09_x40(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                      uint32_t r3);

/* stub DLL 对象 vtable 方法（[+8]/[+0xC]/[+0x10]）：stub 返 0 */
uint32_t zm_dll_init(uc_engine *uc);
uint32_t zm_dll_config(uc_engine *uc, uint32_t a1, uint32_t a2, uint32_t a3);
uint32_t zm_dll_entry(uc_engine *uc, uint32_t a1, uint32_t a2, uint32_t a3);

#endif

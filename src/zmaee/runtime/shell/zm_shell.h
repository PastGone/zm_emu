#ifndef ZM_SHELL_H
#define ZM_SHELL_H

#include <stdint.h>
#include <unicorn/unicorn.h>

/* ZMAEE IShell 原生虚表（g_aee_shell_vtbl @ .data:0x64440，34 槽）处理函数。
 * shell 为全局单例：root.getShell() 返回 SHELL 对象，其 vptr 指向 SHELL_VT_ADDR。
 * 旧 RT_VT / zm_rt_* 是同一张表的早期误命名（CreateInstance/GetDeviceInfo/
 * LoadLibraryExt 均为 IShell 方法），已更名对齐。
 *
 * 与真实虚表的对位（本文件函数 ↔ RE 符号）：
 *   +0x08 CreateInstance  （旧称 queryInterface：按服务号返回子系统对象）
 *   +0x10 GetDeviceInfo   （旧称 getSystemInfo：写屏幕宽高等设备信息）
 *   +0x58 LoadDLL         （RE sub_35230，applet 实测传 "zmsys001.dll"）
 *   +0x5C UnloadDLL       （RE sub_346D8）
 *   +0x78 LoadLibraryExt  （旧称 loadDLL2，applet 用它载 zmsys006.dll）
 * 其余槽接 zm_shell_stub：仅记录日志、返回 0，保证任何槽被调用都不会
 * 落到 "非法的外部调用" 而卡死。
 */

/* 通用 stub：记录 offset 与参数，返回 0 */
uint32_t zm_shell_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                       uint32_t r2, uint32_t r3);

/* +0x00 / +0x04 */
uint32_t zm_shell_AddRef(uc_engine *uc, uint32_t r0);
uint32_t zm_shell_Release(uc_engine *uc, uint32_t r0);

/**
 * +0x08 CreateInstance(this, classID, out_ptr)：按 CLSID 返回子系统对象。
 * CLSID 表严格按 RE 反编译（16777219=0x1000003 起，详见 zm_shell.c）：
 *   0x1000003 IFileMgr → FileMgr     0x1000004 INetMgr  → NETMGR
 *   0x1000005 IDisplay → DISPLAY     0x1000009 ITAPI    → TAPI
 *   0x100000B ISetting → SETTING     0x100000C IMedia(音频) → MEDIA
 *   其余（IGps/IGSensor/IAddrBook/IMemStream/IZip/IStatusBar/IUtil）
 *   尚未实现：*out=0，返回 -3（RE default 语义）。
 */
uint32_t zm_shell_CreateInstance(uc_engine *uc, uint32_t svc, uint32_t out_ptr);

/**
 * +0x10 GetDeviceInfo(this, info_ptr)：写设备信息结构（91 个 dword）。
 *
 * 布局（RE：nativeAEEGetDeviceInfo，按 dword 下标）：
 *   [0]version [1]userid [2]ScreenW [3]ScreenH [4]color_depth [5]dwLang
 *   [6]cap [7]bKbd [8]bTouchScreen [9]nMaxRam
 *   [10..]szCompany [14..]szOS [18..]szModel [66..]szBuildDate
 * 实现先整块清零（与固件 memset 一致），再填 ScreenW/ScreenH（取自
 * AppHeader，保证 applet 布局与渲染窗口一致）与 bTouchScreen=1；
 * 其余字段的枚举语义待 RE，暂保持 0。
 */
uint32_t zm_shell_GetDeviceInfo(uc_engine *uc, uint32_t out_ptr);

/* +0x48 GetTickCount(this)：单调毫秒时间戳（复用 zm_root_get_tick /
 * SDL_GetTicks） */
uint32_t zm_shell_GetTickCount(uc_engine *uc);

/* +0x30 GetApplet(this, index)：返回当前 applet 实例对象。
 * RE（nativeAEERepaint）：GetApplet(shell, 0) 返回当前 applet 对象，
 * 随后调 (*applet_vt+8)(applet, 4, 0, 0) 触发重绘；applet_vt[+8] 即
 * 事件回调（事件码 4=重绘），与 create_cbk 的 handler 机制一致。
 * 实现直接返回 g_instance（create_cbk 时记录）。 */
uint32_t zm_shell_GetApplet(uc_engine *uc, uint32_t index);

/* +0x3C SetTimer / +0x40 CancelTimer / +0x44 CancelOwnerTimer 及到期
 * 派发（sub_34394）已拆至 ../timer/zm_timer.h（zm_timer_*）。 */

/**
 * +0x58 LoadDLL(this, name_ptr, name_len, out_ptr)（RE sub_35230）
 *
 * sub_85248 调 (*SHELL+0x58)(SHELL, "zmsys001.dll", 28, &out)。
 * 本实现为 stub：读取 dll 名仅作日志，把 DLL_OBJ 写入 *out_ptr 并返回，
 * 让 applet 后续对 DLL 对象 vt[+8/+0xC/+0x10] 的调用不崩溃。
 */
uint32_t zm_shell_LoadDLL(uc_engine *uc, uint32_t name_ptr, uint32_t name_len,
                          uint32_t out_ptr);

/* +0x5C UnloadDLL(this, handle)（RE sub_346D8）。stub：log + 返 0。 */
uint32_t zm_shell_UnloadDLL(uc_engine *uc, uint32_t handle);

/**
 * +0x78 LoadLibraryExt(this, buf, size, out_obj_ptr, ...)（旧称 loadDLL2）
 *
 * sub_83E24 调 (*SHELL+0x78)(SHELL, buf, 20, &v2[1], a1[116], a1[117])。
 * 语义：按名载入模块（zmsys006.dll），把模块对象指针写入 *out_obj_ptr，
 * 返回非 0 表成功。applet 随后检查返回值与 *out_obj_ptr 均非 0 才继续，
 * 再调 (*out_obj_ptr)->vt[0x0C](...)。
 *
 * 本实现：写 *out_obj_ptr=0 并返回 0（失败）→ sub_83E24 返回 nullptr →
 * sub_83F50 返回 false → sub_8433C 返回 false → sub_82AB0 走 sub_82424
 * 绘制 applet 自身 UI（DLL 真实执行需单独工程）。
 */
uint32_t zm_shell_LoadLibraryExt(uc_engine *uc, uint32_t r0, uint32_t buf,
                                 uint32_t size, uint32_t out_obj_ptr);

/* 服务对象 release（NETMGR_VT_ADDR[+4] / TAPI_VT_ADDR[+4]）：无操作返 0 */
uint32_t zm_svc_release(uc_engine *uc);

/* NETMGR_VT_ADDR[+0x1C]：stub */
uint32_t zm_netmgr_x1C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                       uint32_t r3);

/* TAPI_VT_ADDR[+0x2C] / [+0x40]：stub */
uint32_t zm_tapi_x2C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                     uint32_t r3);
uint32_t zm_tapi_x40(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                     uint32_t r3);

/* ISetting（0x100000B，g_aee_setting_vtbl @ .data:0x64408，14 槽）通用
 * stub —— 各槽语义待 RE。 */
uint32_t zm_setting_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                         uint32_t r2, uint32_t r3);

/* ISetting[+0x24]（RE sub_33F4C）。此前被误当作"audio.getStatus"实现。
 * 保留既有行为：向 out4 与 out_buf[0..2] 写 0 并返回 0 —— 00000405 的
 * sub_83D44 依赖该返回值与输出做后续分支判断（与 instance[122..124]
 * 比较），改掉会改变 applet 走向。 */
uint32_t zm_setting_x24(uc_engine *uc, uint32_t out4, uint32_t out_buf);

/* stub DLL 对象 vtable 方法（loadDLL 返回的 DLL_OBJ） */
uint32_t zm_dll_init(uc_engine *uc);
uint32_t zm_dll_config(uc_engine *uc, uint32_t a1, uint32_t a2, uint32_t a3);
uint32_t zm_dll_entry(uc_engine *uc, uint32_t a1, uint32_t a2, uint32_t a3);

#endif /* ZM_SHELL_H */

/* IUtil（0x1000013）的观测探针，见 zm_shell.c */
uint32_t zm_util_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                      uint32_t r2, uint32_t r3);

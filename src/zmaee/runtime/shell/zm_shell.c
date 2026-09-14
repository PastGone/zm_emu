#include "zm_shell.h"

#include "../../../emu.h" /* g_instance（GetApplet 返回当前实例） */
#include "../../../log/log.h"
#include "../../../tool/uc_helper.h"
#include "../../core/zm_root.h" /* zm_root_get_tick（SDL_GetTicks） */
#include "../../core/zm_str.h"  /* read_cstr */
#include "../../audio/zm_audio.h" /* zm_audio_set_sound_*：声音开关真正落地 */
#include "../../../event.h"       /* zm_event_request_close：applet 请求关闭 */
#include <stdint.h>
#include <string.h> /* memset（GetDeviceInfo 整块清零） */

/* =========================================================================
 * ZMAEE IShell 原生虚表处理函数（g_aee_shell_vtbl @ .data:0x64440，34 槽）
 *
 * shell 为全局单例：root.getShell() 返回 G_SHELL_ADDR 对象（SHIM+0x100），
 * 其 vptr 指向 SHELL_VT_ADDR（SHIM+0x180）。旧 RT_VT / zm_rt_* 是早期误命名，
 * 已更名对齐（CreateInstance/GetDeviceInfo/LoadLibraryExt 均为 IShell 方法）。
 *
 * 原则：任何槽被调用都不应落到 "非法的外部调用" 而卡死 pause_console。
 *   - 实测过行为的槽（CreateInstance/GetDeviceInfo/LoadDLL/UnloadDLL/
 *     LoadLibraryExt/GetTickCount）按真实行为实现；
 *   - 其余接 zm_shell_stub：仅记录日志、返回 0。
 * ========================================================================= */

/* 通用 stub：记录 offset 与参数，返回 0（不崩） */
uint32_t zm_shell_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                       uint32_t r2, uint32_t r3) {
  (void)uc;
  log_debug("shell stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1,
            r2, r3);
  return 0;
}

/* +0x00 AddRef：单例返回 1 */
uint32_t zm_shell_AddRef(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 1;
}

/* +0x04 Release：无操作 */
uint32_t zm_shell_Release(uc_engine *uc, uint32_t r0) {
  (void)uc;
  (void)r0;
  return 0;
}

/* +0x08 CreateInstance(this, classID, out_ptr)
 * CLSID 表严格按 RE 反编译 ZMAEE_IShell_CreateInstance：
 *   16777219=0x1000003 IFileMgr     16777220=0x1000004 INetMgr
 *   16777221=0x1000005 IDisplay     16777222=0x1000006 IGps
 *   16777223=0x1000007 IGSensor     16777225=0x1000009 ITAPI
 *   16777226=0x100000A IAddrBook    16777227=0x100000B ISetting(变体)
 *   16777228=0x100000C IMedia       16777230=0x100000E IMemStream
 *   16777231=0x100000F IZip         16777232=0x1000010 IStatusBar
 *   16777235=0x1000013 IUtil        default: *out=0, 返回 -3
 * （该变体无 0x1000008/0xD/0x11/0x12。）
 * 尚无对象实现的 CLSID 按 default 语义失败（*out=0，-3），applet 可优雅回退。 */
uint32_t zm_shell_CreateInstance(uc_engine *uc, uint32_t svc, uint32_t out_ptr) {
  uint32_t outobj = 0;
  int32_t ret = 0;
  switch (svc) {
  case 0x1000003: /* IFileMgr */
    outobj = G_FileMgr_ADDR;
    break;
  case 0x1000004: /* INetMgr */
    outobj = G_NETMGR_ADDR;
    break;
  case 0x1000005: /* IDisplay 全局单例（原生 g_aee_display_vtbl；旧 GFX/GFX_VT
                     是同一张表的早期误命名，已并入 display） */
    outobj = DISPLAY;
    break;
  case 0x1000009: /* ITAPI */
    outobj = G_TAPI_ADDR;
    break;
  case 0x100000B: /* ISetting（RE：ZMAEE_ISetting_New） */
    outobj = SETTING;
    break;
  case 0x100000C: /* IMedia = 音频（RE：ZMAEE_IMedia_New，用户确认） */
    outobj = G_MEDIA_ADDR;
    break;
  case 0x1000006: /* IGps      —— 占位模拟对象（方法走 trap 观测） */
    outobj = G_GPS_ADDR;
    break;
  case 0x1000007: /* IGSensor  —— 占位模拟对象 */
    outobj = G_GSENSOR_ADDR;
    break;
  case 0x100000A: /* IAddrBook —— 占位模拟对象 */
    outobj = G_ADDRBOOK_ADDR;
    break;
  case 0x100000E: /* IMemStream—— 占位模拟对象 */
    outobj = G_MEMSTREAM_ADDR;
    break;
  case 0x1000010: /* IStatusBar—— 占位模拟对象 */
    outobj = G_STATUSBAR_ADDR;
    break;
  case 0x100000F: /* IZip —— 返回模拟对象地址（vtable→ZIP_VT_ADDR，方法走 zm_zip_stub） */
    outobj = ZIP_ADDR;
    break;
  case 0x1000013: /* IUtil —— 实测每帧都被请求（拿不到就优雅回退 -3）。
                   * 给真实对象会改变 applet 的代码路径（实测出现一次
                   * UC_ERR_INSN_INVALID），所以**默认仍返回 -3**，
                   * 仅在 ZM_IUTIL=1 时给出真实对象用于观测。 */
    if (getenv("ZM_IUTIL") && getenv("ZM_IUTIL")[0] == '1') {
      outobj = G_IUTIL_ADDR;
    } else {
      outobj = 0;
      ret = -3; /* RE default 语义 */
    }
    break;
  default:
    outobj = 0;
    ret = -3; /* RE default 语义 */
    break;
  }
  if (out_ptr)
    uc_write32(uc, out_ptr, outobj);
  log_info("IShell.CreateInstance(svc=0x%X) -> obj=0x%X ret=%d", svc, outobj,
           ret);
  return (uint32_t)ret;
}

/* +0x10 GetDeviceInfo：填设备信息结构（RE：nativeAEEGetDeviceInfo）
 * 结构为 91 个 dword，固件调用前先 memset 0x168 清零，再逐项格式化：
 *   [0]version  [1]userid  [2]width  [3]height
 *   [4]color_depth（经 nativeColorDepthToString）
 *   [5]dwLang（经 nativeLanguageToString）
 *   [6]cap      [7]bKbd   [8]bTouchScreen  [9]nMaxRam
 *   [10..13]szCompany  [14..17]szOS  [18..65]szModel  [66..]szBuildDate
 *
 * 此前只写前 16 字节就返回，剩余约 348 字节留作客户机脏数据——其中
 * [8] bTouchScreen 若为脏值/0，applet 可能据此关闭触摸。现先整块清零
 * （与固件一致），再填确定项；语义未定的字段保持 0（即固件 memset 值）。
 */
#define ZM_DEVICE_INFO_DWORDS 91
uint32_t zm_shell_GetDeviceInfo(uc_engine *uc, uint32_t out_ptr) {
  uint8_t zeros[ZM_DEVICE_INFO_DWORDS * 4];
  memset(zeros, 0, sizeof(zeros));
  uc_mem_write(uc, out_ptr, zeros, sizeof(zeros)); /* 与固件 memset 同款清零 */

  /* 确定项：屏幕宽高（RE 中 resolution = %dx%d 取 [2]、[3]）。
   * 关键：这里必须返回**模拟器实际可绘制尺寸**（= 层缓冲 LAYER_W×LAYER_H），
   * 而不是 .app 表头的 ScreenW/ScreenH。applet（如 000004fe sub_F824）会把
   * 它写进 CBK_OBJ+0x110/+0x114 当作绘制 surface 的宽高；若报成表头的
   * 800×800，而我们的层只有 240×320，applet 按 800 宽算坐标就会冲出层缓冲
   * （实测崩在 sub_8F20 往 0x9201E0 写像素）。 */
  uc_write32(uc, out_ptr + 4 * 2, LAYER_W);
  uc_write32(uc, out_ptr + 4 * 3, LAYER_H);
  /* 确定项：本设备是触摸屏（语义明确；置 0 会让 applet 关闭触摸交互） */
  uc_write32(uc, out_ptr + 4 * 8, 1); /* [8] bTouchScreen */

  /* 语义待 RE 的字段（保持 memset 的 0，不臆造）：
   *   [4] color_depth —— RE 已查到 nativeAEEGetDeviceInfo 会调用
   *       ZMAEE_IDisplay_GetBaseLayerDepth（0002671C；CODE XREF 里明确标了
   *       ZMAEE_IShell_GetDeviceInfo+46），说明色深与显示子系统同源；但
   *       返回值还要过一层 nativeColorDepthToString 才落到这一格，该函数的
   *       映射（1/2/4 → 什么枚举）尚未确定，故暂不写。
   *   [5] dwLang      —— 需 nativeLanguageToString 确定枚举
   *   [6] cap / [7] bKbd / [9] nMaxRam / 各字符串字段 */
  log_debug("GetDeviceInfo -> %ux%u (bTouchScreen=1)", LAYER_W, LAYER_H);
  return 0;
}

/* +0x48 GetTickCount：单调毫秒时间戳 */
uint32_t zm_shell_GetTickCount(uc_engine *uc) {
  return zm_root_get_tick(uc); /* SDL_GetTicks */
}

/* +0x30 GetApplet：返回当前 applet 实例（g_instance，create_cbk 时记录） */
uint32_t zm_shell_GetApplet(uc_engine *uc, uint32_t index) {
  (void)index; /* 固件固定用 index=0 取当前 applet */
  return g_instance;
}

/* 定时器（+0x3C/+0x40/+0x44 及派发 sub_34394）已拆至 ../timer/zm_timer.c */

/* +0x58 LoadDLL（RE sub_35230）：stub，返回 DLL_OBJ */
uint32_t zm_shell_LoadDLL(uc_engine *uc, uint32_t name_ptr, uint32_t name_len,
                          uint32_t out_ptr) {
  char name[64];
  uint32_t n = name_len < sizeof(name) - 1 ? name_len : sizeof(name) - 1;
  read_cstr(uc, name_ptr, name, n + 1);
  name[n] = '\0';
  log_info("IShell.LoadDLL(\"%s\", len=%u) -> DLL_OBJ (stub)", name, name_len);
  if (out_ptr)
    uc_write32(uc, out_ptr, DLL_OBJ);
  return DLL_OBJ; /* 非 0 表成功 */
}

/* +0x5C UnloadDLL（RE sub_346D8）：stub */
uint32_t zm_shell_UnloadDLL(uc_engine *uc, uint32_t handle) {
  (void)uc;
  log_info("IShell.UnloadDLL(0x%X) stub", handle);
  return 0;
}

/* +0x78 LoadLibraryExt（旧称 loadDLL2）
 * sub_83E24 用它载入 zmsys006.dll：返回非 0 且 *out_obj_ptr 非 0 才算成功，
 * 随后调 (*out_obj_ptr)->vt[0x0C]。
 *
 * 返回 0（失败）使 sub_83E24 返回 nullptr → sub_83F50 返回 false →
 * sub_8433C 返回 false → sub_82AB0 走 sub_82424 绘制 applet 自身 UI。
 * （DLL 真实执行需单独工程；此处让 applet 回退到自带 UI 绘制路径。） */
uint32_t zm_shell_LoadLibraryExt(uc_engine *uc, uint32_t r0, uint32_t buf,
                                 uint32_t size, uint32_t out_obj_ptr) {
  (void)r0;
  (void)buf;
  (void)size;
  (void)uc;
  if (out_obj_ptr)
    uc_write32(uc, out_obj_ptr, 0); /* out=0 → sub_83E24 判定失败 */
  log_info("IShell.LoadLibraryExt(out_ptr=0x%X, size=%u) -> 0 (fail, applet 自绘 UI)",
           out_obj_ptr, size);
  return 0; /* 0 表失败 → applet 走自带绘制路径 */
}

/* ---- 服务对象（CreateInstance 返回的 G_NETMGR_ADDR / G_TAPI_ADDR） ---- */

/* NETMGR_VT_ADDR[+4] / TAPI_VT_ADDR[+4] release */
uint32_t zm_svc_release(uc_engine *uc) {
  (void)uc;
  return 0;
}

/* NETMGR_VT_ADDR[+0x1C] stub */
uint32_t zm_netmgr_x1C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                       uint32_t r3) {
  (void)uc;
  log_info("stub netmgr[0x1C] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

/* TAPI_VT_ADDR[+0x2C] stub */
uint32_t zm_tapi_x2C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                     uint32_t r3) {
  (void)uc;
  log_info("stub tapi[0x2C] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

/* TAPI_VT_ADDR[+0x40] stub */
uint32_t zm_tapi_x40(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                     uint32_t r3) {
  (void)uc;
  log_info("stub tapi[0x40] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

/* TAPI 其余 18 个槽位的观测探针：仅打日志、返回 0，便于按参数签名反推用途。 */
uint32_t zm_tapi_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                      uint32_t r2, uint32_t r3) {
  (void)uc;
  log_debug("tapi stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1, r2,
            r3);
  return 0;
}

/* ZMAEE IZip 5 个槽位的观测探针。 */
uint32_t zm_zip_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                     uint32_t r2, uint32_t r3) {
  (void)uc;
  log_debug("zip stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1, r2,
            r3);
  return 0;
}

/* ---- ISetting（0x100000B，g_aee_setting_vtbl @ .data:0x64408，14 槽）----
 * 各槽语义待 RE（+0x00 sub_33D60、+0x04 sub_34118、+0x08 sub_34050、
 * +0x0C sub_33D74、+0x10 sub_33D78、+0x14 sub_33D7C、+0x18 sub_33FB4、
 * +0x1C sub_33E2C、+0x20 sub_33EFC、+0x24 sub_33F4C、+0x28 sub_33D80、
 * +0x2C sub_33D84、+0x30 sub_33E98、+0x34 sub_33DFC）。 */
uint32_t zm_setting_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                         uint32_t r2, uint32_t r3) {
  (void)uc;
  log_debug("setting stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1,
            r2, r3);
  return 0;
}

/* ISetting[+0x18] = **声音开关**（把设置键 "on" 置为 r1，再转给系统侧）
 *
 * RE（固件 sub_33FB4 @0x33FB4，挂在 g_aee_setting_vtbl +0x18）：
 *   env->NewStringUTF("on");
 *   b = AEEJNIBridge.obtainBundle();
 *   putBundleInt(b, "on", r1);
 *   postMessageToJava(5, b);      // 消息 5 = 设置变更
 *   return 0;
 * 00000506 启动时调它**两次、两次 r1 都是 1**（applet 自己把声音设为"开"；
 * 它多传的那两个函数指针真机只当多余实参忽略）。
 *
 * 真机上"听不听得到"由 Java 侧音量 + applet 开关共同决定；在模拟器里我们让
 * applet 开关说了算（默认出声，ZM_SOUND=0 才强制静音），而落地那一枪是
 * IMedia 的 pauseMusic/resumeMusic（+0x18/+0x1C，见 zm_audio.c）——所以这里
 * 仍然只记录偏好，真正的静音/恢复交给那两支。 */
uint32_t zm_setting_set_sound(uc_engine *uc, uint32_t on) {
  (void)uc;
  /* 注意值语义：非 0 = 关声音（见 zm_audio.c 顶部说明，实测 00000506）。 */
  log_info("ISetting[0x18] 声音开关: 值=%u（真机转给 Java 侧调媒体音量；"
           "本 app 约定 非0=关/0=开，由 zm_audio 扮演 Java 侧落地）",
           on);
  zm_audio_set_sound_flag(on);
  return 0;
}

/* ISetting[+0x24]：保留既有"写 0"行为（详见 zm_shell.h 注释） */
uint32_t zm_setting_x24(uc_engine *uc, uint32_t out4, uint32_t out_buf) {
  if (out4)
    uc_write32(uc, out4, 0);
  if (out_buf) {
    /* out_buf 至少 3 个 dword */
    uc_write32(uc, out_buf, 0);
    uc_write32(uc, out_buf + 4, 0);
    uc_write32(uc, out_buf + 8, 0);
  }
  return 0;
}

/* ---- stub DLL 对象 vtable 方法（loadDLL 返回的 DLL_OBJ） ---- */

uint32_t zm_dll_init(uc_engine *uc) {
  (void)uc;
  log_info("stub dll init(+8)");
  return 0;
}

uint32_t zm_dll_config(uc_engine *uc, uint32_t a1, uint32_t a2, uint32_t a3) {
  (void)uc;
  log_info("stub dll config(+0xC) a1=%u a2=%u a3=%u", a1, a2, a3);
  return 0;
}

uint32_t zm_dll_entry(uc_engine *uc, uint32_t a1, uint32_t a2, uint32_t a3) {
  (void)uc;
  log_info("stub dll entry(+0x10) a1=%u a2=%u a3=%u", a1, a2, a3);
  return 0;
}

/* IDisplay 之外的 IUtil：7 个槽全部接探针。
 * 目的不是"实现",而是**实测出 applet 到底调哪几个槽、传什么参数** ——
 * 这样 IUtil 的用途是观测出来的，不依赖任何异构建的反编译。 */
uint32_t zm_util_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                      uint32_t r2, uint32_t r3) {
  static uint32_t cnt[8];
  uint32_t i = off / 4u;
  if (i < 8u)
    cnt[i]++;
  static uint32_t total = 0;
  if ((++total % 300u) == 0u) {
    log_info("[IUtil] 累计 %u 次调用，各槽次数：", total);
    for (uint32_t k = 0; k < 8u; k++)
      if (cnt[k])
        log_info("   +0x%02X : %u 次", k * 4u, cnt[k]);
  }
  static uint32_t shown = 0;
  if (shown++ < 12)
    log_info("[IUtil] 槽+0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1,
             r2, r3);
  (void)uc;
  return 0;
}

/* IShell[+0x24] = CloseApplet(bRetToIdle)
 *
 * RE（固件 sub_3482C）：里面那句日志串就是 "CloseApplet: bRetToIdle = %d"。
 * 00000506 在标题页点"退出"那一块（约 (96~107, 219~225)）会调它 —— 以前我们
 * 当成未知槽（zm_shell_stub 返回 0），点了毫无反应。
 *
 * 现在：先派发 EV_STOP(evt=1) 让 applet 跑完自己的退出回调（sub_9270：
 * ISetting(0) 关声音、pauseMusic、取消定时器、释放 UI 并存盘 data/farm），
 * 收尾后由 TR_enter_event_loop 分支检测到"已请求关闭"而结束模拟。 */
uint32_t zm_shell_CloseApplet(uc_engine *uc, uint32_t b_ret_to_idle) {
  (void)uc;
  log_info("IShell.CloseApplet(bRetToIdle=%u) → applet 请求关闭自己", b_ret_to_idle);
  zm_event_request_close();
  return 0;
}

#ifndef ZM_DLL_H
#define ZM_DLL_H

/* ============================================================================
 * ZMAEE DLL 子系统（IShell.LoadDLL / LoadLibraryExt）—— **预留模块，暂未实现**
 *
 * 这里只放"侦察结果 + 将来实现路线"，代码实体还没写。等真要支持某个 DLL
 * 时，照本文档开工，不用重新挖一遍。
 *
 * 结论：目前**不影响游戏类 applet**。唯一已知使用者是 00000506 的**计费**流程
 * （zmsys001.dll），而它要连的服务器早已下线，真跑通也只是"计费失败"。
 * ==========================================================================*/

/* ---------------------------------------------------------------------------
 * 1. 现状（我们是怎么"假装"的）
 *   - IShell.LoadDLL  → zm_shell_LoadDLL：不读文件，直接返回一个假的 DLL 对象
 *                       （DLL_OBJ 地址，见 emu_svc_traps.h 的 ZM_DLL_OBJ_VT）。
 *   - IShell.UnloadDLL→ zm_shell_UnloadDLL：空实现，返回 0。
 *   - IShell.LoadLibraryExt（旧称 loadDLL2）→ 返回 0 表失败，让 applet 回退到
 *                       自带 UI 绘制路径（见 zm_shell.c 该函数注释）。
 *   - DLL 对象的槽我们只接了：+0x04 Release（返回 0）、+0x08 init、
 *                       +0x0C config、+0x10 entry（三个都只记日志）。
 *     ⇒ applet 的"扩展 DLL"逻辑一概没真正执行。
 * -------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * 2. 已探明的目标：zmsys001.dll = **计费模块**
 *   applet/lib/zmsys001.dll（45 KB）。同目录另有 zmsys001.so（12.9 KB）——
 *   头部 word0 不同（0x1772 vs 0x1771），是另一个 ABI/平台的变体，暂不管。
 *
 *   导出 API（构建 DLL 对象时要用这些名字换成函数地址）：
 *     zmaeecharge_Init / zmaeecharge_UiEvent / zmaeecharge_Repaint
 *     zmaeecharge_Billreq_Entry / zmaeecharge_Cardreq_Entry
 *     zmaeecharge_Release / zmaeecharge_RegSmsfilter
 *
 *   它依赖的**固件 API**（我们得提供可调用地址）：
 *     AEE_ITAPI_GetICCID            → 我们 ITapi 侧目前是 stub
 *     AEE_ITAPI_SetActiveSimCard    → 同上
 *     AEE_InetMgr_OpenHttp          → INetMgr 侧 stub
 *     AEE_IHttp_SendRequest         → 无实现
 *     AEE_IShell_SetTimer           → 已实现（定时器子系统）
 *
 *   它读写的文件：conf\aeemint.dat、smkey.dat、mspyl.date
 *   它访问的网络：aeefee.zmapp.com，路径 /billreq
 *                （Content-Type: application/octet-stream；HTTP POST）
 *   它处理的事件码：ZMAEE_EV_PEN_UP / ZMAEE_EV_USER / ZMAEE_EV_SUSPEND /
 *                  ZMAEE_EV_RESUME / ZMAEE_EV_STOP
 *                  另有 ZMAEE_CHARGE_UINOTIFY / ZMAEE_HTTP_EV_FAILURE
 *   ⇒ 说明这个 DLL 自己收 UI 事件、自己重绘（不是纯后端）。
 * -------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * 3. 容器格式（实测四个 DLL 全部吻合）
 *     0x000  u32  类型/版本 = 0x1770 | type
 *                 实测：zmsys001.dll=0x1771  zmsys002.dll=0x1772
 *                       zmsys006.dll=0x1776  zmsys001.so =0x1772
 *     0x004  u32  标志位（实测 0x1C / 0x20 / 0x21 / 0x24）
 *     0x008  u32  0
 *     0x00C  u32  载荷大小
 *     0x010--0x17F  全 0
 *     0x180         **载荷起点**（ARM 代码；实测 0x180 + 载荷大小 = 文件大小 - 8）
 *     文件末尾 8 字节  收尾/校验（内容未查明）
 *   载荷内部同时含代码与 rodata —— 实测字符串落在 0x540..0x97F0，均在载荷范围内。
 *   导出表 / 导入表 / 重定位表的**具体布局尚未确定**（见 §4 的阻塞点）。
 * -------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * 4. 固件加载流程与阻塞点
 *
 *   流程（RE：IShell.LoadDLL = 固件 sub_35230，ARM 态）：
 *     读文件 → memset(buf,0,0x100) → 分配句柄 → [helper 解析容器]
 *     → 查导出符号 **"zmaee_main"** → 调用它 → 返回值 (+7) & ~7 = DLL 对象指针
 *     （即：zmaee_main 返回该 DLL 的接口表/对象，固件再把它交给 applet 使用）
 *
 *   阻塞点：LoadDLL 调的那几个 helper 走的是**惰性绑定 thunk 表**：
 *       add ip, pc, #0, #12 ; add ip, ip, #0x56000 ; ldr pc, [ip, #off]!
 *   顺着 thunk 算出的表项，在文件里**全是同一个 resolver 地址 0xA710**
 *   → 运行期才解析，静态读不出容器内部的表结构。
 *
 *   绕开的办法：让**固件自己解析**——在模拟器里跑到 LoadDLL，把它解析后的
 *   内存 dump 出来反推表结构（比静态读文件靠谱）。
 * -------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * 5. 00000506 付费流程实测（ZM_STAT=1 抓的，点购买按钮 (139,113) 触发）
 *     stub cbk_default
 *     IShell.LoadDLL("zmsys001.dll")
 *     stub dll init(+0x08)
 *     stub dll config(+0x0C) a1=28
 *     stub dll entry(+0x10) a1=0 …
 *     dll Release(+0x04) ×35      ← 原先未接（"非法的外部调用"），已接上
 *   applet 侧调用点：sub_19A84（旁边就是 "zmsys001.dll" 串）——
 *     R0=[this+4]; R1=[[R0]+4]; BLX R1; STR 0,[this+4]  = 标准 Release 惯用法
 * -------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * 6. 将来实现的三条路线（建议先 (b)+(c)）
 *   (a) 真加载：解析容器 → 映射载荷到客户机 → 重定位 →
 *       导入表接到我们的跳板（需要"名字 → 跳板地址"表）→
 *       导出表构造成 DLL 对象。工作量最大；且 DLL 会连已下线的服务器。
 *   (b) 按导出语义假实现：Init 返回成功、UiEvent/Repaint 挂到我们的显示层、
 *       Billreq_Entry/Cardreq_Entry 返回失败/无网络 → 让 applet 走自己的
 *       "支付失败/取消"分支，付费流程闭环且有合理提示。
 *   (c) 只做 (a) 的前半：把容器格式 dump 出来存档，再决定值不值得真跑。
 * -------------------------------------------------------------------------*/

#endif /* ZM_DLL_H */

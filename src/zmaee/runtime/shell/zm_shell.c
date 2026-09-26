#include "zm_shell.h"
#include "zm_dll.h" /* ZMAEE DLL 预留模块：侦察结果与将来实现路线（见文件头） */

#include "../../../emu.h"	/* g_instance（GetApplet 返回当前实例） */
#include "../../../event.h" /* zm_event_request_close：applet 请求关闭 */
#include "../../../log/log.h"
#include "../../../tool/uc_helper.h"
#include "../../../ulibc/include/u_heap.h" /* u_malloc：客户机里放目录字符串 

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
#include "../../audio/zm_audio.h"		   /* zm_audio_set_sound_*：声音开关真正落地 */
#include "../../core/zm_root.h"			   /* zm_root_get_tick（SDL_GetTicks） */
#include "../../core/zm_str.h"			   /* read_cstr */
#include <stdint.h>
#include <string.h> /* memset（GetDeviceInfo 整块清零） */

/* 通用 stub：记录 offset 与参数，返回 0（不崩） */
uint32_t
zm_shell_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
	(void)uc;
	log_debug("shell stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1, r2, r3);
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
	log_info("IShell.CreateInstance(svc=0x%X) -> obj=0x%X ret=%d", svc, outobj, ret);

	/* ---- 识别 applet 自己的上下文对象，并把 [CBK_OBJ+0x48] 指过去 ----
	 * 证据（00000001）：
	 *   sub_319C：ctx = malloc(0x90)，[ctx] = &unk_18148（它自己的类表）
	 *   sub_418(ctx)：[ctx+0x54] = getShell()；紧接着
	 *                 CreateInstance(IDisplay) 把结果写进 [ctx+0x58] ← 就是这里
	 *   随后 sub_319C 再填 [ctx+0x64]=IFileMgr / +0x68=IMedia / +0x6C=ISetting
	 *   / +0x84=label。
	 *   引擎侧 sub_3E54 构造 UI 对象时读的正是这同一批偏移
	 *   （+0x58→obj+0x18、+0x64→obj+0x1C、+0x68→obj+0x20、+0x84→obj+0x10）。
	 * 我们以前把 [CBK_OBJ+0x48] 指向自己仿造的 CBK_CTX（布局不对：我们把
	 * +0x54/+0x58 当成宽/高写），于是构造读到的 +0x84 恒为 0 → 该类 getter
	 * 返回 NULL → 被当 this → 崩在 pc=0x1EDC。
	 * 判定条件是两个精确值：out_ptr 落在对象 +0x58、且 +0x54 == shell。 */
	/* ---- 识别 applet 自己的上下文对象，并把 [CBK_OBJ+0x48] 指过去 ----
	 * 判定时机选在**最后一个服务** ISetting(0x100000B) 返回时（applet 依次往
	 * +0x64 IFileMgr、+0x68 IMedia、+0x6C ISetting 里写），此时三个字段都应
	 * 已就位 —— 三重校验，不会误判到别的对象。
	 * 打开后 00000001 的 NULL-this 崩溃（pc=0x1EDC）消失，前进到下一个缺口。
	 * 设 ZM_APPCTX=0 可关闭（对照用）。 */
	if (svc == 0x100000B && out_ptr && (!getenv("ZM_APPCTX") || getenv("ZM_APPCTX")[0] != '0')) {
		uint32_t media = uc_read32(uc, out_ptr - 4); /* ctx+0x68 */
		uint32_t fmgr = uc_read32(uc, out_ptr - 8);	 /* ctx+0x64 */
		if (media == G_MEDIA_ADDR && fmgr == G_FileMgr_ADDR) {
			uint32_t ctx = out_ptr - 0x6C;
			uc_write32(uc, CBK_OBJ + 0x48, ctx);
			log_info("识别到 applet 上下文 0x%X（三重校验：IFileMgr/IMedia/ISetting "
					 "就位）：[CBK_OBJ+0x48] 已指过去",
					 ctx);
		}
	}

	/* ★ 第二套上下文布局（00000502/000004051 这类）——**延迟确认**。
	 *
	 * 它们从不请求 IMedia，上面那条三重校验永不命中 ✗ —— [CBK_OBJ+0x48] 一直
	 * 是我们自己的假对象 CBK_CTX，而 applet 拿它当自己的上下文用
	 * （0x16064 就是 `return [CBK_OBJ+0x48]`），于是：
	 *   [ctx+0x60] = 0 → 0x1AD00/0x19208 里 r5 = [[ctx+0x60]+0x10] = 0
	 *   → 0x19194 造的类表条目 +8 恒为 0（"未就绪"）→ 0xE854 返回 NULL
	 *   → CRT(0x494) 把 0 写进 obj+0x258 → 0x354 读得 0 → 读地址 0 的 blob 头
	 *     （AppletID 0x4E9）→ `表 + 表[0x20]` 相对派发算出垃圾 → 崩 0x80E291E8。
	 *
	 * 这套 applet 的 IDisplay 写入带同样特征（落点 ctx+0x58、前一个 dword 是
	 * shell）：实测 00000502 out_ptr=0xED00AC → ctx=0xED0054。
	 * 但**不能立刻写** ✗：00000506 的 IDisplay 写入早于它自己的 ISetting 识别，
	 * 抢先写会把 [CBK_OBJ+0x48] 指向半成品上下文而崩在 0x7B64（实测两次，加
	 * "非栈区"/"当前值仍是 CBK_CTX" 两条约束都挡不住）。
	 * 因此：先只**记下候选**，等下一次服务调用时交叉验证（+0x54 是 shell 且
	 * +0x58 是 IDisplay 对象）才真正指过去。 */
	/* ★ 未解决缺口（00000502/000004051）：[CBK_OBJ+0x48] 的纠正。
	 *
	 * 它们**从不请求 IMedia**，上面那条三重校验永不命中 ✗ —— [CBK_OBJ+0x48] 始终
	 * 是我们自己的假对象 CBK_CTX，而 applet 拿它当自己的上下文用
	 * （0x16064 就是 `return [CBK_OBJ+0x48]`），于是：
	 *   [ctx+0x60] = 0 → 0x1AD00/0x19208 处 r5 = [[ctx+0x60]+0x10] = 0
	 *   → 0x19194 造的类表条目 +8 恒为 0（"未就绪"）→ 0xE854 返回 NULL
	 *   → CRT(0x494) 把 0 写进 obj+0x258 → 0x354 读得 0 → 读地址 0 的 blob 头
	 *     （AppletID 0x4E9）→ `表 + 表[0x20]` 相对派发算出垃圾 → 崩 0x80E291E8。
	 *
	 * 【三次尝试均撤回，勿再照抄】按「IDisplay 落点 = ctx+0x58 且前一个 dword 是
	 * shell」补识别（含"非栈区"、"当前值仍是 CBK_CTX"、"延迟一轮再确认"、
	 * "只对没请求过 IMedia 的 applet 启用"四种加强）：
	 *   1) 对 00000502 确实算出了它的上下文 0xED0054（实测 out_ptr=0xED00AC）；
	 *   2) 但 00000506 **也**满足全部判据（它同样不请求 IMedia），被指过去后崩在
	 *      0x7B64 —— 说明这套判据无法把两者分开；
	 *   3) 更关键：即使指到 00000502 的"真"上下文，[ctx+0x60] **依然是 0** ✗，
	 *      所以崩溃并没有被绕过 —— 说明 [ctx+0x60] 的填充另有条件（先有鸡还是
	 *      先有蛋），需要先把这条读链的**填充时机**搞清楚，再谈纠正 [CBK_OBJ+0x48]。 */
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
/* ★ 真机（参考 libaee.so.c.txt:58583 ZMAEE_IShell_GetDeviceInfo）是
 *     memset(a2, 0, **0x168u**);
 * 即 **360 字节**（90 个 dword），不是 364！applet 传进来的缓冲常常就是它自己的
 * 栈帧，实测 00000502 的 0x1455C 帧正好 0x168 字节 —— 我们原来写 91 dword
 * = 364 字节，多出的 4 字节正好盖掉调用者 `push {r4,r5,r6,lr}` 保存的 r4 ✗。
 * 后果：该函数一返回 r4 就变 0 → 后续 `ldr r0,[r4,#0x2c]` 读地址 0 的 blob 头
 * （AppletID 0x4E9）当对象表 → blx 到 0x2E000000 崩。
 * 逐指令实证：PC=0x14604（bl 前一瞬）R4=0x82244 → PC=0x14608（返回后）R4=0x0。 */
#define ZM_DEVICE_INFO_BYTES 0x168u
/* ---- 目录类接口的返回：真机这些"get dir"槽返回的是**字符串指针**（参考
 * ZMAEE_IShell_New 里就 `RootDir = ZMAEE_GetRootDir(); strcpy(byte_65C6C, RootDir)`）。
 * 我们以前全是桩（返回 0），于是 applet 拼路径时前缀为空 → 最终只打开 ".dat" ✗。
 * 这里在 applet 堆里放一份目录字符串并返回其客户机地址。 */
static uint32_t g_dir_ptr = 0;
static uint32_t zm_shell_dir_string(uc_engine *uc, const char *s) {
	if (!g_dir_ptr)
		g_dir_ptr = u_malloc(uc, 256);
	if (!g_dir_ptr)
		return 0;
	size_t n = strlen(s);
	if (n > 200)
		n = 200;
	uc_mem_write(uc, g_dir_ptr, s, n + 1);
	return g_dir_ptr;
}
uint32_t zm_shell_GetRootDir(uc_engine *uc) {
	/* 模拟器把 applet 目录当作根；返回空串比臆造盘符安全（applet 自带 "c:"） */
	return zm_shell_dir_string(uc, "");
}
uint32_t zm_shell_GetWorkDir(uc_engine *uc) {
	return zm_shell_dir_string(uc, "");
}
uint32_t zm_shell_GetAppDir(uc_engine *uc, uint32_t buf, uint32_t cap) {
	uint32_t p = zm_shell_dir_string(uc, "");
	(void)buf;
	(void)cap;
	return p;
}
uint32_t zm_shell_GetDeviceInfo(uc_engine *uc, uint32_t out_ptr) {
	uint8_t zeros[ZM_DEVICE_INFO_BYTES];
	memset(zeros, 0, sizeof(zeros));
	uc_mem_write(uc, out_ptr, zeros, sizeof(zeros)); /* 与固件 memset 同款清零（0x168） */

	/* 以下字段全部照抄参考 ZMAEE_IShell_GetDeviceInfo（58583 起）：
	 *   *(dword*)a2      = 108      结构大小
	 *   *((dword*)a2+2/3)= 屏宽/屏高
	 *   *((dword*)a2+6)  = 4383     cap
	 *   *((dword*)a2+7)  = 1        bKbd
	 *   *((dword*)a2+8)  = 1        bTouchScreen
	 *   *((dword*)a2+9)  = 2048000  nMaxRam
	 * （[1] UserID / [4] BaseLayerDepth / [5] / 各字符串字段暂留 0，不臆造。） */
	uc_write32(uc, out_ptr + 4 * 0, 108);

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
	/* 参考里固定写的其余字段（照抄，避免 applet 读到 0 走别的分支） */
	uc_write32(uc, out_ptr + 4 * 6, 4383);	  /* [6] cap */
	uc_write32(uc, out_ptr + 4 * 7, 1);		  /* [7] bKbd */
	uc_write32(uc, out_ptr + 4 * 9, 2048000); /* [9] nMaxRam */

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
/* +0x58 LoadDLL。
 * 目前是**假实现**：不读 DLL 文件，直接返回一个空的 DLL_OBJ（+0x08 init /
 * +0x0C config / +0x10 entry 三个槽都只记日志）。
 *
 * 真实现该做什么、目标 DLL（zmsys001.dll = 计费模块）的容器格式、固件
 * sub_35230 的加载流程、以及阻塞点，全部记在 zm_dll.h（预留模块）里。 */
uint32_t zm_shell_LoadDLL(uc_engine *uc, uint32_t name_ptr, uint32_t name_len, uint32_t out_ptr) {
	char name[64];
	uint32_t n = name_len < sizeof(name) - 1 ? name_len : sizeof(name) - 1;
	read_cstr(uc, name_ptr, name, n + 1);
	name[n] = '\0';
	log_info(
		"IShell.LoadDLL(\"%s\", len=%u) -> DLL_OBJ (stub，真实现见 zm_dll.h)", name, name_len);
	if (out_ptr)
		uc_write32(uc, out_ptr, DLL_OBJ);
	return DLL_OBJ; /* 非 0 表成功 */
}

/* +0x5C UnloadDLL（RE sub_346D8）：stub（真实现见 zm_dll.h） */
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
uint32_t zm_shell_LoadLibraryExt(
	uc_engine *uc, uint32_t r0, uint32_t buf, uint32_t size, uint32_t out_obj_ptr) {
	(void)r0;
	(void)buf;
	(void)size;
	(void)uc;
	if (out_obj_ptr)
		uc_write32(uc, out_obj_ptr, 0); /* out=0 → sub_83E24 判定失败 */
	log_info("IShell.LoadLibraryExt(out_ptr=0x%X, size=%u) -> 0 (fail, applet 自绘 UI)",
			 out_obj_ptr,
			 size);
	return 0; /* 0 表失败 → applet 走自带绘制路径 */
}

/* ---- 服务对象（CreateInstance 返回的 G_NETMGR_ADDR / G_TAPI_ADDR） ---- */

/* NETMGR_VT_ADDR[+4] / TAPI_VT_ADDR[+4] release */
uint32_t zm_svc_release(uc_engine *uc) {
	(void)uc;
	return 0;
}

/* NETMGR_VT_ADDR[+0x1C] stub */
uint32_t zm_netmgr_x1C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
	(void)uc;
	log_info("stub netmgr[0x1C] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
	return 0;
}

/* TAPI_VT_ADDR[+0x2C] stub */
uint32_t zm_tapi_x2C(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
	(void)uc;
	log_info("stub tapi[0x2C] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
	return 0;
}

/* TAPI_VT_ADDR[+0x40] stub */
uint32_t zm_tapi_x40(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
	(void)uc;
	log_info("stub tapi[0x40] r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
	return 0;
}

/* TAPI 其余 18 个槽位的观测探针：仅打日志、返回 0，便于按参数签名反推用途。 */
uint32_t
zm_tapi_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
	(void)uc;
	log_debug("tapi stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1, r2, r3);
	return 0;
}

/* ZMAEE IZip 5 个槽位的观测探针。 */
uint32_t
zm_zip_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
	(void)uc;
	log_debug("zip stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1, r2, r3);
	return 0;
}

/* ---- ISetting（0x100000B，g_aee_setting_vtbl @ .data:0x64408，14 槽）----
 * 各槽语义待 RE（+0x00 sub_33D60、+0x04 sub_34118、+0x08 sub_34050、
 * +0x0C sub_33D74、+0x10 sub_33D78、+0x14 sub_33D7C、+0x18 sub_33FB4、
 * +0x1C sub_33E2C、+0x20 sub_33EFC、+0x24 sub_33F4C、+0x28 sub_33D80、
 * +0x2C sub_33D84、+0x30 sub_33E98、+0x34 sub_33DFC）。 */
uint32_t
zm_setting_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
	(void)uc;
	log_debug("setting stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1, r2, r3);
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
uint32_t
zm_util_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
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
		log_info("[IUtil] 槽+0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1, r2, r3);
	(void)uc;
	return 0;
}

/* IShell[+0x24] = CloseApplet(bRetToIdle)
 *
 * RE（固件 sub_3482C）：里面那句日志串就是 "CloseApplet: bRetToIdle = %d"。
 * 00000506 在标题页点"退出"那一块（约 (96~107, 219~225)）会调它 —— 以前我们
 * 当成未知槽（zm_shell_stub 返回 0），点了毫无反应。
 *
 * 现在：先派发 APP_CMD_DESTROY(evt=1) 让 applet 跑完自己的退出回调（sub_9270：
 * ISetting(0) 关声音、pauseMusic、取消定时器、释放 UI 并存盘 data/farm），
 * 收尾后由 TR_enter_event_loop 分支检测到"已请求关闭"而结束模拟。 */
uint32_t zm_shell_CloseApplet(uc_engine *uc, uint32_t b_ret_to_idle) {
	(void)uc;
	log_info("IShell.CloseApplet(bRetToIdle=%u) → applet 请求关闭自己", b_ret_to_idle);
	zm_event_request_close();
	return 0;
}

#ifndef EMU_H
#define EMU_H

#include <stddef.h>
#include <stdint.h>

#include "./tool/paser_info.h"
#include <capstone/capstone.h>
#include <unicorn/unicorn.h>

// -------------------- 内存布局常量 --------------------
#define ONE_MB (0x100000U)
#define HALF_MB (0x80000U)
//
/* payload 在客户机中的映射基址。
 *
 * 关键：applet 的**绝对地址体系就是"文件偏移"**，payload 必须映射到低地址。
 * 证据（00000506）：
 *   1) payload 内的字面量池项是 IDA 标注的相对表达式，例如
 *        off_18D68 DCD loc_188 - 0x18B38   （实际值 0xFFFE7650）
 *      按 VA = 文件偏移 还原得到目标 0x3C0（sub_3A0 的指令），198/198 项全部
 *      落在 payload 范围内；而若按 VA = 偏移 + 0x80000 还原则一项都对不上。
 *   2) 入口 stub（文件偏移 0x188）经 ROOT 槽写入的 handler = 0x11108C，
 *      它是 applet 自己算出的绝对地址；只有 VA = 文件偏移时该地址才落在
 *      payload 内部（否则读到的是未映射内存里的全 0，执行后 PC 飞出）。
 *
 * 因此这里保持 0：applet 被映射到 [0, payload_size)，其内部绝对地址直接可用。
 * （0 页不映射，payload 实际落在 [0x1000, payload_size) 的映射区间内。） */
#define BLOB_BASE (0x0U)
#define BLOB_SIZE (1 * ONE_MB)

#define STACK_BASE (BLOB_BASE + BLOB_SIZE)
#define STACK_SIZE (1 * HALF_MB)
#define STACK_TOP (STACK_BASE + STACK_SIZE)

#define HEAP_BASE (STACK_TOP + ONE_MB / 8)
#define HEAP_SIZE (6 * ONE_MB)
#define HEAP_END (HEAP_BASE + HEAP_SIZE)

#define SHIM_BASE (HEAP_END)
#define SHIM_SIZE (1 * HALF_MB)

#define TRAMP_BASE (SHIM_BASE + SHIM_SIZE)
#define TRAMP_SIZE (1 * HALF_MB)

//
#define ROOT_SLOT_OFF 0x180U
#define APPLET_ENTRY_OFF 0x188U
#define APPLET_ENTRY_POINT (BLOB_BASE + APPLET_ENTRY_OFF)

/* -------------------- shim 虚表地址定义 -------------------- */
/* ---- 客户机可见的"显示层"像素缓冲 ----
 *
 * applet 通过 IDisplay::GetLayerInfo(disp, 1, &info) 取 layer_info，其中
 *   info[0xC] = 宽、info[0x10] = 高（已实测确认），
 *   info[0x24] = **层像素缓冲指针**（逆向 sub_10248 @0x10318-0x1033C：
 *     surf[0x0]=info[0xC] 宽、surf[0x4]=info[0x10] 高、surf[0x8]=1(格式)、
 *     surf[0x1C]=info[0x24] → 随后把 surf 作为 BitBlt 的源）。
 *
 * 模拟器把这块缓冲放在 SHIM 区（0x8000 起，避开已用的表区），布局为
 * RGB565、宽高 LAYER_W×LAYER_H；applet 直接读写它，present 时再转成
 * ARGB 上传到 SDL 纹理显示。 */
#define LAYER_W 240
#define LAYER_H 320
#define LAYER_BUF (SHIM_BASE + 0x8000U)
#define LAYER_BUF_SIZE (LAYER_W * LAYER_H * 2)

/* IBitmap 的像素/调色板区（RE：ZMAEE_IBitmap_New 里 v5[9] = v5 + 11，
 * 即像素指针 = 对象 + 44，对象与像素是连续分配的一整块）。
 * 我们是单例对象，无法照搬"对象+44"的布局（BITMAP 后面 0x80 字节就是
 * BITMAP_VT，放不下像素），故单独开一块，把 +36 像素指针指过去。
 * 地址必须避开 LAYER_BUF(0x8000..0x2D800) 与 TRAMP(0x80000)。 */
/* 解码像素池（客户机可见）：IImage 解码出的像素必须真落在 guest 内存，
 * 因为 applet 自带的 GDI 是直接按 IBitmap 的 +36 像素指针去读的，
 * 宿主侧那份 rgba 它根本看不到。
 * RE 依据：ZMAEE_IBitmap_GetInfo 就是 `memcpy(out, bitmap + 8, 32)`，
 * 即 out[7] = bitmap[36] = 像素指针。
 * 布局：IBitmap 对象字段 8 个 dword（+8..+40）+ 像素数据。
 * 0x30000..0x80000 共 320KB，避开 LAYER_BUF(0x8000..0x2D800)。循环复用。 */
#define FRAMEBUF (SHIM_BASE + 0x30000U)
#define FRAMEBUF_SIZE (LAYER_W * LAYER_H * 2)

/* 解码像素池。位置后移，给 FRAMEBUF 腾出 0x30000..0x55800。 */
#define PIX_POOL (SHIM_BASE + 0x56000U)
#define PIX_POOL_SIZE 0x2A000U

#define ROOT (SHIM_BASE + 0x000U)
/* SHELL 必须避开 ROOT 的函数指针表区。
 * emu.c 把整个 SHIM 按 4 字节步长填成 TRAMP_BASE+i，因此 ROOT 的表从
 * 0x000 起连续铺开，已知最高槽 +0x154（create_cbk）→ 表区至少
 * 0x000..0x158。SHELL 原先在 0x100、字段延伸到 0x218，正好压住
 * ROOT 的 0x100..0x158 段（+0x154 create_cbk 落在 SHELL+0x54），
 * 一旦 applet 写 shell 字段就会破坏该槽。现从 0x200 起，给 ROOT
 * 表留出完整 0x200 字节。 */
#define SHELL (SHIM_BASE + 0x200U)
/* SHELL_VT 必须避开 SHELL 的对象字段区。
 * RE 依据（ZMAEE_IShell_ActiveApplet）：IShell 对象是「vptr + 数据字段」，
 * 字段至少到 +0x118（读 a1+276 与 a1+280），故对象区约 0x100..0x220。
 * 虚表原先放在 0x180，正好压在对象 +0x80..+0x108 的字段上——一旦 applet
 * 直接访问 shell 字段（SDK 内联访问器常见）就会重写虚表，与当初 CBK_OBJ
 * 踩碎 DISPLAY vptr 的崩溃同型。现移到 0x240（34 槽 → 0x2C8，不与
 * FileMgr@0x300 冲突）。 */
/* 对象字段区约 0x200..0x31F，虚表独立放 0x400（34 槽 → 0x488，
 * 与 FileMgr@0x500 不冲突） */
#define SHELL_VT (SHIM_BASE + 0x400U) /* g_aee_shell_vtbl @ .data:0x64440，34 槽 */
#define FileMgr (SHIM_BASE + 0x500U)
#define FileMgr_VT (SHIM_BASE + 0x580U) /* 16 槽 → 0x5C0 */
#define FILE1 (SHIM_BASE + 0x600U)
#define FILE_VT (SHIM_BASE + 0x680U) /* 10 槽 → 0x6A8 */
#define DUMMY_BUF (SHIM_BASE + 0x700U) /* scratch；至 INIT_CTX@0x800 有 0x100 余量（原 0x750 头顶仅 0xB0） */
/* 0x100000B ISetting（RE：g_aee_setting_vtbl @ .data:0x64408，14 槽）。
 * 旧名 AUDIO 是误命名：该对象被用于 +0x14 / +0x24，曾按"音频状态"实现；
 * 真实接口是 ISetting（配置读写），音频是下面的 MEDIA。 */
#define SETTING (SHIM_BASE + 0x1800U)
#define SETTING_VT (SHIM_BASE + 0x1880U) /* 14 槽 → 0x18B8 */
/* 0x100000C IMedia = 音频（用户确认；RE：g_aee_media_vtbl @ .data:0x640E4，
 * 25 槽）。旧名 AP 是误命名。 */
#define MEDIA (SHIM_BASE + 0x1900U)
#define MEDIA_VT (SHIM_BASE + 0x1980U) /* 25 槽 → 0x19E4 */

//

/* 00000405.app 新增 shim 对象地址（0x800 起，与 DUMMY_BUF@0x750 不冲突） */
#define INIT_CTX (SHIM_BASE + 0x800U) /* 256B 零填充：init 事件 r3 上下文 */
/* ---- 服务对象区（每个对象/VT 间隔 0x100+，见下方布局说明）----
 * 布局教训（00001b62 实测）：applet 会把 root.create_cbk 返回的
 * CBK_OBJ 当 ≥0x170 字节的大上下文结构体用（+0x48 存 SHELL、+0x4C 起
 * 存 CreateInstance 服务对象表、+0x128 起填句柄数组）。真实固件里
 * 各对象在 RAM 中相距甚远，互不干扰；此前的紧凑布局（0x900~0xC00
 * 挤 7 个对象）被 applet 上下文写入踩碎 DISPLAY vptr / DISPLAY_VT /
 * DLL_OBJ_VT，导致读回空指针崩溃。现按每对象 0x800~0x100 间隔拉开。 */
#define NETMGR (SHIM_BASE + 0x900U) /* 0x1000004 INetMgr 服务对象 */
#define NETMGR_VT (SHIM_BASE + 0x980U)
#define TAPI (SHIM_BASE + 0xA00U) /* 0x1000009 ITAPI 服务对象 */
#define TAPI_VT (SHIM_BASE + 0xA80U)
#define CBK_OBJ (SHIM_BASE + 0xB00U)    /* create_cbk 返回对象；
                                           applet 当大上下文用，留 0x800 */
#define CBK_OBJ_VT (SHIM_BASE + 0x1300U) /* 可写：applet 覆写 vt[+8] */
#define DLL_OBJ (SHIM_BASE + 0x1400U)    /* loadDLL 返回的 stub DLL 对象 */
#define DLL_OBJ_VT (SHIM_BASE + 0x1480U)

/* ---- ZMAEE IDisplay / IBitmap 原生虚表（逆向实测 g_aee_display_vtbl /
 * g_aee_bitmap_vtbl @ .data:0x63E10 / 0x63DF4）----
 * display 是全局单例，由 queryInterface(0x1000005) 返回。旧 GFX/GFX_VT
 * 是早期对同一张表的误命名（实测偏移与本表吻合），已并入此处。
 * bitmap 由 IDisplay.CreateBitmap/LoadBitmap 创建，这里用单个 BITMAP 单例
 * 作为所有 bitmap 对象的 vtable 模板（真实多实例后续再扩展）。 */
/* ---- create_cbk 的"应用上下文"结构体（00000506 实测）----
 *
 * ROOT[0x154] create_cbk 返回 CBK_OBJ；applet 随后把 **CBK_OBJ+0x48**
 * 当成一个上下文指针来用（getter sub_8884，实测 31 个调用点）：
 *     bl   sub_8884          ; r0 = *(CBK_OBJ + 0x48)
 *     ldr  r0, [r0, #0x50]   ; → IDisplay*（存进资源管理器的 +0x58，
 *                            ;   随后以 vt[0xA8]=CreateImage 调用）
 *     ldr  r0, [r0, #0x54]   ; → 屏幕宽
 *     ldr  r0, [r0, #0x58]   ; → 屏幕高
 * 宽/高的用法是 `add r0,r0,r0,lsr#31; asr r0,r0,#1`（除以 2，配合
 * ldrsh 取坐标做居中计算），证实是标量尺寸而非对象。
 *
 * 该字段若留空（值为 build_vtables 填的 trap 地址），applet 取到的
 * display 就是野值 → 资源管理器 +0x58 为 0 → 解引用崩溃。
 * 地址放在 MEDIA_VT(0x1980+25*4=0x19E4) 之后、IMAGE_POOL(0x2000) 之前。 */
#define CBK_CTX (SHIM_BASE + 0x1A00U)
/* CBK_CTX 是**数据区**，必须像 LAYER_BUF 一样排除在 SHIM 的 trap 地址填充
 * 之外并清零。否则 applet 的懒创建逻辑
 *     ldr r0,[ctx,#0x8c]; cmp r0,#0; bne <直接使用>; bl <创建>
 * 会拿到 trap 地址（0x821A8C）当成真实对象解引用 → 崩溃。
 * 未使用字段保持 0，applet 才会走"创建"分支。 */
#define CBK_CTX_SIZE 0x100U

/* IDisplay 对象。需要装下：+0 vptr、+8 活动层索引、+20 每层一个字节的
 * 标志数组、+36 起 16 个 52 字节的层项 —— 合计 ≈ 936 字节。
 * 原先放在 0x1500（到 DISPLAY_VT@0x1600 只剩 0x100）根本不够，层项会被
 * 虚表吃掉。现移到 0x1B00..0x1F00（CBK_CTX 之后、IMAGE_POOL 之前）。 */
#define DISPLAY (SHIM_BASE + 0x1B00U)
#define DISPLAY_OBJ_SIZE 0x400U   /* 全局 display 对象（0x1000005） */
#define DISPLAY_VT (SHIM_BASE + 0x1600U) /* 58 槽 ×4B = 0xE8 */
#define BITMAP (SHIM_BASE + 0x1700U)     /* bitmap 单例对象（CreateBitmap 旧桩） */
#define BITMAP_VT (SHIM_BASE + 0x1780U)  /* 7 槽 ×4B = 0x1C */

/* ---- ZMAEE ImageEntry：IDisplay::CreateImage 返回的**数据对象** ----
 * 逆向（00000506 sub_3644 → sub_37B4 → vt[0xAC] BitBlt）：
 *   entry  = CreateImage(disp, alloc, free, &entry)
 *   entry->vt[8] : SetData(entry, 0, name, strlen(name))   ← 文件名，非文件句柄
 *   entry->vt[28]: Decode(entry, alloc, free, &surf, 0)    ← 出绘制用 surface
 *   entry[4]  : 逐帧偏移表指针
 *   entry[8]  : 解码后像素基址（surface，传给 BitBlt 的就是这个）
 *   entry[0x38]: 名字/相对路径（"%s\\%s" 的第二个 %s）
 * 即 BitBlt 的 surface 参数 = entry+8（真图形对象），不是 entry 本身。
 * 注意：游戏里绝大多数 BitBlt 的 surface 其实是 applet **自己在栈上构造**
 * 的 ZMAEE_GDI_Surface（{宽,高,位深,透明色}+像素），见 zm_image.h。
 * 两者都用 IMAGE_POOL 的槽（entry 在前，surf 紧随其后一个槽），
 * 因此池容量按“每张图 2 个槽”规划。 */
#define IMAGE_ENTRY_OFF_SURF 0x08U

/* ---- IImage / 解码 IBitmap 对象池 ----
 * IDisplay::CreateImage(alloc, free, &out) 造的 IImage 与
 * IImage::Decode 解出的 IBitmap 在真实固件里是**堆对象**（尺寸/格式随图变），
 * applet 只经虚表使用、不摸字段，因此这里用固定地址池 + 宿主侧记录表实现多实例
 * （旧实现返回 0/单例，导致 applet 拿到空指针直接崩）。
 * 池区从 0x2000 起（SETTING/MEDIA 之后），每对象 0x40 字节。 */
/* 0x1000013 IUtil 服务对象。
 * 实测 applet **每帧**都请求它一次（拿不到就返回 -3 优雅回退）—— 是唯一
 * "每帧都在失败"的服务，因此值得给它一个真实对象，看它到底想调哪个槽。
 * 真机 util 虚表是 7 个槽（0x00~0x18）。位置选在 DISPLAY(0x1B00，约 0x360 字节)
 * 之后、IMAGE_POOL(0x2000) 之前的空隙里。 */
#define IUTIL (SHIM_BASE + 0x1F00U)
#define IUTIL_VT (SHIM_BASE + 0x1F80U) /* 7 槽 ×4B = 0x1C */

#define TR_util_x00 TRAP(IUTIL_VT + 0x00U)
#define TR_util_x04 TRAP(IUTIL_VT + 0x04U)
#define TR_util_x08 TRAP(IUTIL_VT + 0x08U)
#define TR_util_x0C TRAP(IUTIL_VT + 0x0CU)
#define TR_util_x10 TRAP(IUTIL_VT + 0x10U)
#define TR_util_x14 TRAP(IUTIL_VT + 0x14U)
#define TR_util_x18 TRAP(IUTIL_VT + 0x18U)

#define IMAGE_POOL (SHIM_BASE + 0x2000U)  /* 64 × 0x40 = 0x1000 */
#define IMAGE_VT (SHIM_BASE + 0x3000U)    /* 32 槽 ×4B = 0x80 */
#define IMAGE_SLOT_SIZE 0x40U
#define IMAGE_SLOT_COUNT 64
#define BITMAP_POOL (SHIM_BASE + 0x3100U) /* 64 × 0x40 = 0x1000 */
#define BITMAP_SLOT_SIZE 0x40U
#define BITMAP_SLOT_COUNT 64

//
#define SIZE_SLOT                                                              \
  (SHIM_BASE + 0x700U) // 其实这个文件大小槽还有待确认，applet 会写入这个槽
#define API_SLOT                                                               \
  (SHIM_BASE + 0x710U) // applet 会写入这个槽，用于调用 applet 接口的 vt

// -------------------- trap 地址宏 --------------------
#define TRAP(idx) (TRAMP_BASE + (idx) - SHIM_BASE)

// -------------------- 外部函数 trap 地址 --------------------

/* -------------------- trap 地址定义 -------------------- */
// root

#define TR_root_getShell TRAP(ROOT + 0x00U)
#define TR_root_malloc TRAP(ROOT + 0x08U)
#define TR_root_free TRAP(ROOT + 0x0cU)
#define TR_root_str_copy TRAP(ROOT + 0x20U)
#define TR_root_sprintf TRAP(ROOT + 0x6cU)
#define TR_root_str_ctor TRAP(ROOT + 0x88U)
#define TR_root_spec_lookup TRAP(ROOT + 0xa4U)

/* ROOT+0xB0 = zmaee_strstr(haystack, needle)：子串查找。
 * applet 用它判断资源名后缀（如 strstr(name, ".zbmp")），
 * 未实现会导致 .zbmp 资源被误判成 png → 走错加载分支而崩溃。 */
#define TR_root_strstr TRAP(ROOT + 0xb0U)
#define TR_root_str_find TRAP(ROOT + 0xa8U)
// runtime
/*


*/
/* ---- ZMAEE IShell 原生虚表（g_aee_shell_vtbl @ .data:0x64440，34 槽）----
 * +0x08 CreateInstance（旧称 queryInterface）、+0x10 GetDeviceInfo（旧称
 * getSystemInfo）、+0x58 LoadDLL（RE sub_35230）、+0x5C UnloadDLL
 * （RE sub_346D8）、+0x78 LoadLibraryExt（旧称 loadDLL2）此前已按行为
 * 实现；其余槽接 zm_shell_stub，保证不落 "非法的外部调用"。 */
#define TR_shell_AddRef TRAP(SHELL_VT + 0x00U)
#define TR_shell_Release TRAP(SHELL_VT + 0x04U)
#define TR_shell_CreateInstance TRAP(SHELL_VT + 0x08U)
#define TR_shell_x0C TRAP(SHELL_VT + 0x0CU) /* RE sub_34DE4，未知 */
#define TR_shell_GetDeviceInfo TRAP(SHELL_VT + 0x10U)
#define TR_shell_GetRootDir TRAP(SHELL_VT + 0x14U)
#define TR_shell_SetWorkDir TRAP(SHELL_VT + 0x18U)
#define TR_shell_GetWorkDir TRAP(SHELL_VT + 0x1CU)
#define TR_shell_StartApplet TRAP(SHELL_VT + 0x20U)
#define TR_shell_x24 TRAP(SHELL_VT + 0x24U) /* RE sub_3482C，未知 */
#define TR_shell_CanStartApplet TRAP(SHELL_VT + 0x28U)
#define TR_shell_ActiveApplet TRAP(SHELL_VT + 0x2CU)
#define TR_shell_GetApplet TRAP(SHELL_VT + 0x30U)
#define TR_shell_x34 TRAP(SHELL_VT + 0x34U) /* RE sub_34764，未知 */
#define TR_shell_x38 TRAP(SHELL_VT + 0x38U) /* RE sub_34C1C，未知 */
#define TR_shell_SetTimer TRAP(SHELL_VT + 0x3CU)
#define TR_shell_CancelTimer TRAP(SHELL_VT + 0x40U)
#define TR_shell_CancelOwnerTimer TRAP(SHELL_VT + 0x44U)
#define TR_shell_GetTickCount TRAP(SHELL_VT + 0x48U)
#define TR_shell_OpenWapBrowser TRAP(SHELL_VT + 0x4CU)
#define TR_shell_x50 TRAP(SHELL_VT + 0x50U) /* RE sub_3474C，未知 */
#define TR_shell_SetEndKeyMask TRAP(SHELL_VT + 0x54U)
#define TR_shell_LoadDLL TRAP(SHELL_VT + 0x58U)
#define TR_shell_UnloadDLL TRAP(SHELL_VT + 0x5CU)
#define TR_shell_GetAppletMask TRAP(SHELL_VT + 0x60U)
#define TR_shell_SetAppletMask TRAP(SHELL_VT + 0x64U)
#define TR_shell_IsLoadGlobalLibrary TRAP(SHELL_VT + 0x68U)
#define TR_shell_LoadGlobalLibrary TRAP(SHELL_VT + 0x6CU)
#define TR_shell_FreeGlobalLibrary TRAP(SHELL_VT + 0x70U)
#define TR_shell_IsGlobalLibraryUseStaticMem TRAP(SHELL_VT + 0x74U)
#define TR_shell_LoadLibraryExt TRAP(SHELL_VT + 0x78U)
#define TR_shell_EntryApplet TRAP(SHELL_VT + 0x7CU)
#define TR_shell_GetAppDir TRAP(SHELL_VT + 0x80U)
#define TR_shell_GetSupportHall TRAP(SHELL_VT + 0x84U)
// fs
/*
 * IFile 虚表（逆向实测：gAEEFileVtbl @ .data:00064010，函数指针均 +1 表示 Thumb）
 *
 *   +0x00  ZMAEE_IFile_AddRef
 *   +0x04  ZMAEE_IFile_Release   ← 本模拟器的 file.close
 *   +0x08  ZMAEE_IFile_Read      ← 本模拟器的 file.read
 *   +0x0C  ZMAEE_IFile_Write     （未接线）
 *   +0x10  ZMAEE_IFile_Readable  （未接线）
 *   +0x14  ZMAEE_IFile_Writeable （未接线）
 *   +0x18  ZMAEE_IFile_Cancel    （未接线）
 *   +0x1C  ZMAEE_IFile_Flush     （未接线）
 *   +0x20  ZMAEE_IFile_Seek      ← 本模拟器的 file.seek
 *   +0x24  ZMAEE_IFile_Tell      ← 本模拟器的 file.tell
 *
 * 关键点：表里**没有** GetSize/Size 槽位。取文件大小的惯用法只能是
 *   Seek(0, SEEK_END) 然后 Tell()（此时位置恰好等于总大小）。
 * 因此 +0x24 必须实现为 Tell（返回当前读写位置）。
 * 之前实现成"返回总大小"是错的 —— 只在"先 seek 到末尾"这一种调用序列下
 * 碰巧正确，在任意位置调用会给出错误结果。
 *
 * 注：seek 的参数序（whence/offset 谁在前）尚无 applet 覆盖验证，
 * 保持现状未改动；若后续有 applet 用到 seek，需用 RE 数据核对。
 */
/* ---- ZMAEE IFileMgr 原生虚表（RE 实测：g_filemgr_vtbl @ .data:00064038，
 * 16 槽；紧随 gAEEFileVtbl @0x64010 之后）----
 *   +0x00 sub_29DE8  +0x04 sub_29DF0   （惯例 AddRef/Release）
 *   +0x08 ZMAEE_IFileMgr_OpenFile
 *   +0x0C sub_2A550  +0x10 sub_2A4E0  +0x14 sub_2A45C  +0x18 sub_2A3F4
 *   +0x1C sub_2A344  +0x20 sub_2A7BC  +0x24 sub_2A2D0  +0x28 sub_29EA0
 *   +0x2C sub_29E7C  +0x30 sub_29E40  +0x34 sub_29E08  +0x38 sub_29E00
 * 注：+0x30 RE 已证伪"enumFile"旧说——实为存储区支持查询
 * （a2: 0→'C'内置盘，1→'E'，>=2→SD 挂载?'T':0）。
 * +0x20 RE=sub_2A7BC：TestFile 存在性检查（ConvertFileName 分派
 * 包内/ assets.zip / 文件系统三路），非目录枚举；枚举槽待 RE。 */
#define TR_fileMgr_AddRef TRAP(FileMgr_VT + 0x00U)
#define TR_fileMgr_Release TRAP(FileMgr_VT + 0x04U)
#define TR_fileMgr_open_file TRAP(FileMgr_VT + 0x08U)
#define TR_fileMgr_x0C TRAP(FileMgr_VT + 0x0CU)  /* RE sub_2A550 */
#define TR_fileMgr_x10 TRAP(FileMgr_VT + 0x10U)  /* RE sub_2A4E0 */
#define TR_fileMgr_x14 TRAP(FileMgr_VT + 0x14U)  /* RE sub_2A45C */
#define TR_fileMgr_x18 TRAP(FileMgr_VT + 0x18U)  /* RE sub_2A3F4 */
#define TR_fileMgr_x1C TRAP(FileMgr_VT + 0x1CU)  /* RE sub_2A344 */
#define TR_fileMgr_x20 TRAP(FileMgr_VT + 0x20U)  /* RE sub_2A7BC（00001b62 高频） */
#define TR_fileMgr_x24 TRAP(FileMgr_VT + 0x24U)  /* RE sub_2A2D0 */
#define TR_fileMgr_x28 TRAP(FileMgr_VT + 0x28U)  /* RE sub_29EA0 */
#define TR_fileMgr_x2C TRAP(FileMgr_VT + 0x2CU)  /* RE sub_29E7C */
#define TR_fileMgr_x30 TRAP(FileMgr_VT + 0x30U)  /* RE sub_29E40（旧称 enumFile） */
#define TR_fileMgr_x34 TRAP(FileMgr_VT + 0x34U)  /* RE sub_29E08 */
#define TR_fileMgr_x38 TRAP(FileMgr_VT + 0x38U)  /* RE sub_29E00 */
#define TR_file_close TRAP(FILE_VT + 0x04U) /* Release */
#define TR_file_read TRAP(FILE_VT + 0x08U)
#define TR_file_seek TRAP(FILE_VT + 0x20U)
#define TR_file_tell TRAP(FILE_VT + 0x24U) /* Tell：返回当前读写位置 */
/* ---- ZMAEE IMedia 原生虚表（RE：g_aee_media_vtbl @ .data:0x640E4，25 槽）
 * ---- IMedia 即音频（用户确认）。偏移逐槽按 RE：
 *   +0x00 sub_32BC8(AddRef)  +0x04 sub_32BCC(Release)  +0x08 sub_32BD0
 *   +0x0C sub_32BD8          +0x10 sub_32E80(play)     +0x14 sub_32D0C(stop)
 *   +0x18 sub_32CE8  +0x1C sub_32CC4  +0x20 sub_32DF8  +0x24 sub_32DD8
 *   +0x28 sub_32DB4  +0x2C sub_32D4C  +0x30 sub_32C60  +0x34 sub_32C00
 *   +0x38 sub_32BDC  +0x3C sub_32BE4  +0x40 sub_32BE8  +0x44 sub_32BF0
 *   +0x48 sub_32BF4  +0x4C sub_32BF8  +0x50 sub_32BFC  +0x54 sub_33128
 *   +0x58 sub_32D44
 * play/stop 已有真实 SDL_mixer 实现；其余接 zm_media_stub。 */
#define TR_media_AddRef TRAP(MEDIA_VT + 0x00U)
#define TR_media_Release TRAP(MEDIA_VT + 0x04U)
#define TR_media_x08 TRAP(MEDIA_VT + 0x08U)
#define TR_media_x0C TRAP(MEDIA_VT + 0x0CU)
#define TR_media_play TRAP(MEDIA_VT + 0x10U)
#define TR_media_stop TRAP(MEDIA_VT + 0x14U)
#define TR_media_x18 TRAP(MEDIA_VT + 0x18U)
#define TR_media_x1C TRAP(MEDIA_VT + 0x1CU)
#define TR_media_x20 TRAP(MEDIA_VT + 0x20U)
#define TR_media_x24 TRAP(MEDIA_VT + 0x24U)
#define TR_media_x28 TRAP(MEDIA_VT + 0x28U)
#define TR_media_x2C TRAP(MEDIA_VT + 0x2CU)
#define TR_media_x30 TRAP(MEDIA_VT + 0x30U)
#define TR_media_x34 TRAP(MEDIA_VT + 0x34U)
#define TR_media_x38 TRAP(MEDIA_VT + 0x38U)
#define TR_media_x3C TRAP(MEDIA_VT + 0x3CU)
#define TR_media_x40 TRAP(MEDIA_VT + 0x40U)
#define TR_media_x44 TRAP(MEDIA_VT + 0x44U)
#define TR_media_x48 TRAP(MEDIA_VT + 0x48U)
#define TR_media_x4C TRAP(MEDIA_VT + 0x4CU)
#define TR_media_x50 TRAP(MEDIA_VT + 0x50U)
#define TR_media_x54 TRAP(MEDIA_VT + 0x54U)
#define TR_media_x58 TRAP(MEDIA_VT + 0x58U)

/* ---- ZMAEE ISetting 原生虚表（RE：g_aee_setting_vtbl @ .data:0x64408，
 * 14 槽）----
 *   +0x00 sub_33D60  +0x04 sub_34118  +0x08 sub_34050  +0x0C sub_33D74
 *   +0x10 sub_33D78  +0x14 sub_33D7C  +0x18 sub_33FB4  +0x1C sub_33E2C
 *   +0x20 sub_33EFC  +0x24 sub_33F4C  +0x28 sub_33D80  +0x2C sub_33D84
 *   +0x30 sub_33E98  +0x34 sub_33DFC
 * 语义待各自 RE；+0x24 保留"写 0 到 out4/out_buf"的既有行为（applet
 * 依赖它做后续分支判断），其余接 zm_setting_stub。 */
#define TR_setting_AddRef TRAP(SETTING_VT + 0x00U)
#define TR_setting_Release TRAP(SETTING_VT + 0x04U)
#define TR_setting_x08 TRAP(SETTING_VT + 0x08U)
#define TR_setting_x0C TRAP(SETTING_VT + 0x0CU)
#define TR_setting_x10 TRAP(SETTING_VT + 0x10U)
#define TR_setting_x14 TRAP(SETTING_VT + 0x14U)
#define TR_setting_x18 TRAP(SETTING_VT + 0x18U)
#define TR_setting_x1C TRAP(SETTING_VT + 0x1CU)
#define TR_setting_x20 TRAP(SETTING_VT + 0x20U)
#define TR_setting_x24 TRAP(SETTING_VT + 0x24U)
#define TR_setting_x28 TRAP(SETTING_VT + 0x28U)
#define TR_setting_x2C TRAP(SETTING_VT + 0x2CU)
#define TR_setting_x30 TRAP(SETTING_VT + 0x30U)
#define TR_setting_x34 TRAP(SETTING_VT + 0x34U)

/* 00000405.app：FS vtable 缺失槽 */
#define TR_fs_enum TRAP(FS_VT + 0x30U) /* FS_VT[0x30]：enumFile */

/* 00000405.app 新增 ROOT vtable trap（索引 23..45） */

/*
 * 实测修正：applet 00000440 实际跳转 0x8A0060，即 ROOT+0x60；
 * 原先写成 ROOT+0x28 导致该 case 永不命中（memset 落到 default 分支）。
 * 注意这一段的偏移与注释普遍对不上，其他条目待逐个用真实 applet 验证。
 */
#define TR_root_memset TRAP(ROOT + 0x60U)
/* +0x50 memcmp（RE zmaee_memcmp @0x363E8，tramp 桩 0xb9b50）；+0x5C memcpy
 * （RE zmaee_memcpy @0x36470，桩 0xb9b40）。00001b62 调用现场+返回值用法确认。 */
#define TR_root_memcmp TRAP(ROOT + 0x50U)
#define TR_root_memcpy TRAP(ROOT + 0x5CU)
/* +0x90 strchr：调用现场 r1='r' + strb 写回，strchr 家族 */
#define TR_root_strchr TRAP(ROOT + 0x90U)
#define TR_root_str_assign TRAP(ROOT + 0x78U) /* str_assign(str_obj, cstr) */
#define TR_root_get_tick TRAP(ROOT + 0xD8U)   /* ROOT[0xD8] */
/* ROOT 导入表的双精度数学函数（00000506 实测，见 zm_root.h 注释）。
 * 这几个槽若缺失会返回 0：sin/cos 为 0 会让极坐标算出的坐标全部塌到
 * 基准点，鱼群/炮弹位置失真，实测表现为"资源加载了却没有任何绘制"。 */
#define TR_root_srand TRAP(ROOT + 0x40U)  /* srand(seed) */
#define TR_root_rand TRAP(ROOT + 0x44U)   /* rand() */
#define TR_root_sqrt TRAP(ROOT + 0x104U) /* (double)->double */
#define TR_root_cos TRAP(ROOT + 0x114U)  /* (double)->double */
#define TR_root_sin TRAP(ROOT + 0x118U)  /* (double)->double */
#define TR_root_x12C TRAP(ROOT + 0x12CU)
#define TR_root_x130 TRAP(ROOT + 0x130U)
#define TR_root_x140 TRAP(ROOT + 0x140U)
#define TR_root_create_cbk TRAP(ROOT + 0x154U)
#define TR_root_x16C TRAP(ROOT + 0x16CU)

/* ROOT+0x68C（经 00000506 实测：r0 指向含 "data" 的对象 0x820600，
 * r1 是个 0x40 字节缓冲，r2=0x28，r3=调用槽地址本身）。
 * 语义按 zmaee 的惰性资源加载（"resourceData" 的包装对象）处理，
 * 见 zm_root_x68C：把对象数据拷进调用方缓冲，并返回对象首字段。 */
#define TR_root_x68C TRAP(ROOT + 0x68CU)

/* ---- 服务对象 / FS / DLL / CBK trap ----
 * 旧「索引 46..57」方案把这些宏挂在 ROOT+0x2E..0x3F 的伪造索引上，与
 * SHIM↔TRAMP 对射派发不符：applet 经对象虚表发起的真实调用落在各自
 * VT 槽位，伪造索引永不命中（与已修的 loadDLL 同病）。现按真实槽位挂接。
 * fs_chdir 的真实槽位未知，暂缺（其 case 已删，待 RE 后补）。 */
#define TR_netmgr_release TRAP(NETMGR_VT + 0x04U)
#define TR_netmgr_x1C TRAP(NETMGR_VT + 0x1CU)
#define TR_tapi_release TRAP(TAPI_VT + 0x04U)
#define TR_tapi_x2C TRAP(TAPI_VT + 0x2CU)
#define TR_tapi_x40 TRAP(TAPI_VT + 0x40U)
#define TR_dll_init TRAP(DLL_OBJ_VT + 0x08U)
#define TR_dll_config TRAP(DLL_OBJ_VT + 0x0CU)
#define TR_dll_entry TRAP(DLL_OBJ_VT + 0x10U)
#define TR_cbk_default TRAP(CBK_OBJ_VT + 0x08U)

/* ---- ZMAEE IDisplay 原生虚表（g_aee_display_vtbl @ .data:0x63E10）----
 * 偏移严格按逆向贴出的表。带「实测」的槽为旧 GFX 路径验证过的行为
 * （gfx 与 display 本是同一张表），其余为按槽序推测命名，语义待 RE 校准。
 * DISPLAY_VT 58 槽止于 +0xE4，更远的偏移（如旧 GFX 路径见过的 +0x114）
 * 不在本表内，属其它对象/越界调用。 */
#define TR_display_AddRef TRAP(DISPLAY_VT + 0x00U)
#define TR_display_Release TRAP(DISPLAY_VT + 0x04U)
#define TR_display_GetMaxLayerCount TRAP(DISPLAY_VT + 0x08U)
#define TR_display_CreateLayer TRAP(DISPLAY_VT + 0x0CU)
#define TR_display_CreateLayerExt TRAP(DISPLAY_VT + 0x10U)
#define TR_display_FreeLayer TRAP(DISPLAY_VT + 0x14U)
#define TR_display_x18 TRAP(DISPLAY_VT + 0x18U) /* 实测被调，功能未知 stub */
#define TR_display_GetLayerInfo TRAP(DISPLAY_VT + 0x1CU)
#define TR_display_clear TRAP(DISPLAY_VT + 0x20U) /* 实测：clear(color) */
#define TR_display_SetLayerPosition TRAP(DISPLAY_VT + 0x24U)
#define TR_display_Update TRAP(DISPLAY_VT + 0x28U)
#define TR_display_fillRectR TRAP(DISPLAY_VT + 0x2CU) /* 实测：fillRect(rect_ptr)，空实现疑似 invalidate */
#define TR_display_GetActiveLayer TRAP(DISPLAY_VT + 0x30U)
#define TR_display_x34 TRAP(DISPLAY_VT + 0x34U) /* 实测被调，功能未知 stub */
#define TR_display_UnlockScreen TRAP(DISPLAY_VT + 0x38U)
#define TR_display_RegisterCustomFont TRAP(DISPLAY_VT + 0x3CU)
#define TR_display_commit TRAP(DISPLAY_VT + 0x40U) /* 实测：commit 提交帧缓冲 */
#define TR_display_GetFontWidth TRAP(DISPLAY_VT + 0x44U)
#define TR_display_getWidth TRAP(DISPLAY_VT + 0x48U) /* 实测：返回屏幕宽度 */
#define TR_display_measureChar TRAP(DISPLAY_VT + 0x4CU) /* 实测：measureChar(gfx, char_ptr, count, width_out, sp[metrics]) */
#define TR_display_DrawText TRAP(DISPLAY_VT + 0x50U) /* 实测吻合：drawText */
#define TR_display_SetTransColor TRAP(DISPLAY_VT + 0x54U)
#define TR_display_SetOpacity TRAP(DISPLAY_VT + 0x58U)
#define TR_display_SetClipRect TRAP(DISPLAY_VT + 0x5CU)
#define TR_display_GetClipRect TRAP(DISPLAY_VT + 0x60U)
#define TR_display_SetPixel TRAP(DISPLAY_VT + 0x64U)
#define TR_display_DrawLine TRAP(DISPLAY_VT + 0x68U)
#define TR_display_DrawRect TRAP(DISPLAY_VT + 0x6CU)   /* 实测吻合：drawRect */
#define TR_display_FillRect TRAP(DISPLAY_VT + 0x70U)   /* 实测吻合：fillRect */
#define TR_display_DrawRoundRect TRAP(DISPLAY_VT + 0x74U)
#define TR_display_DrawCircle TRAP(DISPLAY_VT + 0x78U)
#define TR_display_FillCircle TRAP(DISPLAY_VT + 0x7CU)
#define TR_display_DrawArc TRAP(DISPLAY_VT + 0x80U)
#define TR_display_FillArc TRAP(DISPLAY_VT + 0x84U)
#define TR_display_FillGradientRect TRAP(DISPLAY_VT + 0x88U)
#define TR_display_AlphaBlendRect TRAP(DISPLAY_VT + 0x8CU)
#define TR_display_DrawImage TRAP(DISPLAY_VT + 0x90U)
#define TR_display_DrawBitmap TRAP(DISPLAY_VT + 0x94U)
#define TR_display_DrawBitmapEx TRAP(DISPLAY_VT + 0x98U)
#define TR_display_DrawBitmapFrame TRAP(DISPLAY_VT + 0x9CU)
#define TR_display_CreateBitmap TRAP(DISPLAY_VT + 0xA0U) /* 返回 BITMAP 单例 */
#define TR_display_LoadBitmap TRAP(DISPLAY_VT + 0xA4U)   /* 返回 BITMAP 单例 */
#define TR_display_CreateImage TRAP(DISPLAY_VT + 0xA8U)
#define TR_display_BitBlt TRAP(DISPLAY_VT + 0xACU)
#define TR_display_Flatten TRAP(DISPLAY_VT + 0xB0U)
#define TR_display_StretchBlt TRAP(DISPLAY_VT + 0xB4U)
#define TR_display_DrawAntialiasingLine TRAP(DISPLAY_VT + 0xB8U)
#define TR_display_DrawWLine TRAP(DISPLAY_VT + 0xBCU)
#define TR_display_GetDMLayerHdlr TRAP(DISPLAY_VT + 0xC0U)
#define TR_display_RelevanceLayer TRAP(DISPLAY_VT + 0xC4U)
#define TR_display_Refresh TRAP(DISPLAY_VT + 0xC8U)
#define TR_display_DrawImageExt TRAP(DISPLAY_VT + 0xCCU)
#define TR_display_DrawSysWallPaper TRAP(DISPLAY_VT + 0xD0U)
#define TR_display_DrawBorderText TRAP(DISPLAY_VT + 0xD4U)
#define TR_display_PushAndSetAlphaLayer TRAP(DISPLAY_VT + 0xD8U)
#define TR_display_PopAndRestoreAlphaLayer TRAP(DISPLAY_VT + 0xDCU)
#define TR_display_RotateScreen TRAP(DISPLAY_VT + 0xE0U)

/* ---- ZMAEE IImage 原生虚表（IDisplay::CreateImage 造出的解码器对象）----
 * 槽位按 00000506 sub_313C / 88AB8 等资源加载现场定：
 *   +0x08 SetData(this, 0, name_ptr, len) —— 按文件名装入（0=成功）
 *   +0x10 / +0x14                        —— 无参准备调用（applet 不看返回值）
 *   +0x1C Decode(this, alloc, free, &bmp, 0) —— 解码出 IBitmap（0=成功）
 * 其余槽接 zm_image_stub，保证不落"非法的外部调用"。 */
#define TR_image_AddRef TRAP(IMAGE_VT + 0x00U)
#define TR_image_Release TRAP(IMAGE_VT + 0x04U)
#define TR_image_SetData TRAP(IMAGE_VT + 0x08U)
#define TR_image_x0C TRAP(IMAGE_VT + 0x0CU)
#define TR_image_x10 TRAP(IMAGE_VT + 0x10U)
#define TR_image_x14 TRAP(IMAGE_VT + 0x14U)
#define TR_image_Width TRAP(IMAGE_VT + 0x18U)
#define TR_image_Decode TRAP(IMAGE_VT + 0x1CU)
/* ---- ZMAEE surface 门面虚表（IImage::Decode 的 out 对象）----
 * 对象字段（00000506 运行期确认）：
 *   +0 = vt   +4 = 原始 surface 指针   +8 = 宽   +0xC = 高
 * 关键槽（逆向 sub_388 type1 / sub_4A0）：
 *   +0x10 GetRect(this, out) → 写 int16 矩形 {l,t,r,b}（分派器据此绘制）
 * 其它槽按对象族的常规顺序给安全的空实现/固定值，避免落到"非法外部调用"。
 * 共 21 槽（0x54 字节）。 */
#define SURF_VT (SHIM_BASE + 0x3200U)
#define TR_surf_release TRAP(SURF_VT + 0x00U)  /* 析构 */
#define TR_surf_x04 TRAP(SURF_VT + 0x04U)
#define TR_surf_x08 TRAP(SURF_VT + 0x08U)
#define TR_surf_x0C TRAP(SURF_VT + 0x0CU)
#define TR_surf_getrect TRAP(SURF_VT + 0x10U)  /* GetRect(this,out)：实测使用 */
#define TR_surf_x14 TRAP(SURF_VT + 0x14U)
#define TR_surf_x18 TRAP(SURF_VT + 0x18U)
#define TR_surf_x1C TRAP(SURF_VT + 0x1CU)
#define TR_surf_x20 TRAP(SURF_VT + 0x20U)
#define TR_surf_x24 TRAP(SURF_VT + 0x24U)
#define TR_surf_x28 TRAP(SURF_VT + 0x28U)
#define TR_surf_x2C TRAP(SURF_VT + 0x2CU)
#define TR_surf_x30 TRAP(SURF_VT + 0x30U)
#define TR_surf_x34 TRAP(SURF_VT + 0x34U)
#define TR_surf_x38 TRAP(SURF_VT + 0x38U)
#define TR_surf_x3C TRAP(SURF_VT + 0x3CU)
#define TR_surf_x40 TRAP(SURF_VT + 0x40U)
#define TR_surf_x44 TRAP(SURF_VT + 0x44U)
#define TR_surf_x48 TRAP(SURF_VT + 0x48U)
#define TR_surf_x4C TRAP(SURF_VT + 0x4CU)
#define TR_surf_x50 TRAP(SURF_VT + 0x50U)

#define TR_image_x20 TRAP(IMAGE_VT + 0x20U)
#define TR_image_x24 TRAP(IMAGE_VT + 0x24U)
#define TR_image_x28 TRAP(IMAGE_VT + 0x28U)
#define TR_image_x2C TRAP(IMAGE_VT + 0x2CU)
#define TR_image_x30 TRAP(IMAGE_VT + 0x30U)
#define TR_image_x34 TRAP(IMAGE_VT + 0x34U)
#define TR_image_x38 TRAP(IMAGE_VT + 0x38U)
#define TR_image_x3C TRAP(IMAGE_VT + 0x3CU)

/* ---- ZMAEE IBitmap 原生虚表（g_aee_bitmap_vtbl @ .data:0x63DF4）----
 * +0x0C/0x14/0x18 是 sub_25F78/sub_25F84/sub_25FF8（未知），接 stub。 */
#define TR_bitmap_AddRef TRAP(BITMAP_VT + 0x00U)
#define TR_bitmap_Release TRAP(BITMAP_VT + 0x04U)
#define TR_bitmap_SetTransColor TRAP(BITMAP_VT + 0x08U)
#define TR_bitmap_sub_25F78 TRAP(BITMAP_VT + 0x0CU)
#define TR_bitmap_GetInfo TRAP(BITMAP_VT + 0x10U)
#define TR_bitmap_sub_25F84 TRAP(BITMAP_VT + 0x14U)
#define TR_bitmap_sub_25FF8 TRAP(BITMAP_VT + 0x18U)

//
#define TR_init_callback                                                       \
  TRAP(ROOT + 0x118cU) // 随便写一个位置我想也应该不影响这个叫什么来.初始化回调

// 事件回调因为 apple 是没有主循环的所以要用外部来完成这个主循环
#define TR_enter_event_loop TRAP(ROOT + 0x1180U)

/* TR_init_callback 执行期间，applet 会调用 ROOT+0x1184 把事件循环的入口
 * 传出来（"注册主循环"）。模拟器据此单独调用 handler，而不是依赖那个
 * 已经退化的 LR 约定——实测 00000506 的 init 顺序是
 *   ROOT+0x1184(handler) → ROOT+0x118c() → 返回
 * 所以必须真正注册，否则像 00000506 这类 applet 会在 init 返回后
 * 直接退出（PC 飞出 blob）。 */
#define TR_register_event_loop TRAP(ROOT + 0x1184U)

/* applet 通过它向宿主请求退出（实测 00000506 参数字符串为 "aborted"）。 */
#define TR_abort TRAP(ROOT + 0x0014U)

// -------------------- 全局变量 --------------------
extern uc_engine *g_uc;
extern AppletHeader g_header;
extern uint32_t g_heap_ptr;

/**
 * 客户机堆策略（由 ZM_ULIBC_HEAP 环境变量在 zm_emu_map_memory 中设定）。
 *
 *   1（默认）ulibc 真实堆：u_malloc / u_free，带空闲链表与相邻块合并。
 *            内存真正回收复用，这是 applet 应该跑在的正确环境——
 *            原始固件上 malloc/free 也是真的回收的。
 *            代价：会暴露 applet 潜伏的 UAF / double-free。
 *            但那些是 applet 的真实 bug，本就该暴露，而不是被掩盖。
 *
 *   0         bump 分配器：host_malloc 单调递增，free 为空操作。
 *             **仅用于排查**：当某个 applet 在真实堆下崩溃时，
 *             切到 0 可以确认"崩溃是由内存回收引起的"（即 applet 有
 *             UAF/double-free），而非模拟器接线本身的问题。
 *             长期保留会让内存只增不减，不是正确行为。
 */
extern int g_ulibc_heap;

extern uint32_t g_instance;
extern uint32_t g_handler;
/* applet 通过 ROOT+0x1184 注册的事件循环入口（0 = 未注册） */
extern uint32_t g_registered_loop;
extern int g_trap_pause;
extern int g_disasm;

/* 当前载入 applet 的短名称（如 "00000102.app"），由 main.c 设置，
 * 供 TR_init_callback 写入 applet instance+4。 */
extern char
    g_app_pathname[4096]; // 4096是 linux
                          // 的最长文件名,这里设了这么大是为了防止搞什么摇蛾子

// 用来反汇编用的一组全局变量，之所以是全局变量是因为要不停的复用
extern csh g_cs_handle;
extern cs_insn *g_sc_insn;
extern size_t g_sc_count;
extern uint8_t g_cscode[16];

// -------------------- 函数声明 --------------------

/* 构建所有虚表：把 trap 地址写入客户机虚拟内存中的 shim 区 */
int zm_emu_build_vtables();

/* Unicorn 内存映射（blob/stack/heap/shim/tramp/zmr） */
int zm_emu_map_memory();

/* 载入 applet blob 到 BLOB_BASE */
int zm_emu_load_blob(FILE *fp, const long *applet_size);

/* 注册 Unicorn 钩子（code / unmapped mem / shim mem） */
int zm_emu_add_hooks();

/* 设置初始寄存器，启动 applet（init → 绘制 → 停止） */
int zm_emu_start_applet();

#endif
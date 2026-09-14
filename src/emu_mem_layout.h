#ifndef EMU_MEM_LAYOUT_H
#define EMU_MEM_LAYOUT_H

#include "emu_mem_regions.h" /* 纯地址算术：各 region 基址/尺寸 */
#include "emu_obj_layout.h"  /* 对象区：服务对象实例/结构体 */

/* ============================================================
 *                        虚 表 区  (0x00000)
 *
 * ROOT_TABLE 是 applet stub 通过 shim 计算的表地址；
 * 其余是各服务对象 / ZMAEE 原生对象的虚表。
 * ============================================================ */
#define ROOT_TABLE_ADDR (SHIM_VT_BASE + 0x000U)

/* IShell 虚表（RE：g_aee_shell_vtbl @ .data:0x64440，34 槽）
 * 必须避开 G_SHELL_ADDR 对象字段区（至少到 +0x118）。
 * 放在 0x400，34 槽 → 0x488。 */
#define SHELL_VT_ADDR (SHIM_VT_BASE + 0x400U)

#define FileMgr_VT_ADDR (SHIM_VT_BASE + 0x500U) /* 16 槽 → 0x540 */
#define FILE_VT_ADDR (SHIM_VT_BASE + 0x580U)    /* 10 槽 → 0x5A8 */

#define NETMGR_VT_ADDR (SHIM_VT_BASE + 0x800U) /* 8 槽 → 0x820 */
#define TAPI_VT_ADDR (SHIM_VT_BASE + 0x880U)   /* 8 槽 → 0x8A0 */

#define CBK_OBJ_VT_ADDR (SHIM_VT_BASE + 0x1000U) /* 可写：applet 覆写 vt+8 */
#define DLL_OBJ_VT_ADDR (SHIM_VT_BASE + 0x1100U)

/* ---- ZMAEE IDisplay / IBitmap 原生虚表（逆向实测 g_aee_display_vtbl /
 * g_aee_bitmap_vtbl @ .data:0x63E10 / 0x63DF4）----
 * display 是全局单例，由 queryInterface(0x1000005) 返回。旧 GFX/GFX_VT
 * 是早期对同一张表的误命名（实测偏移与本表吻合），已并入此处。
 * bitmap 由 IDisplay.CreateBitmap/LoadBitmap 创建，这里用单个 BITMAP 单例
 * 作为所有 bitmap 对象的 vtable 模板（真实多实例后续再扩展）。 */
#define DISPLAY_VT_ADDR (SHIM_VT_BASE + 0x1600U) /* 58 槽 → 0x16E8 */
#define BITMAP_VT_ADDR (SHIM_VT_BASE + 0x1700U)  /* 7 槽 → 0x171C */

/* 0x100000B ISetting（RE：g_aee_setting_vtbl @ .data:0x64408，14 槽）。
 * 旧名 AUDIO 是误命名：该对象被用于 +0x14 / +0x24，曾按"音频状态"实现；
 * 真实接口是 ISetting（配置读写），音频是下面的 G_MEDIA_ADDR。 */
#define SETTING_VT_ADDR (SHIM_VT_BASE + 0x1800U)

/* 0x100000C IMedia = 音频（RE：g_aee_media_vtbl @ .data:0x640E4，25 槽）。
 * 旧名 AP 是误命名。 */
#define MEDIA_VT_ADDR (SHIM_VT_BASE + 0x1900U)

/* 0x1000013 IUtil 服务对象。真机 util 虚表是 7 个槽（0x00~0x18）。 */
#define IUTIL_VT_ADDR (SHIM_VT_BASE + 0x1A00U)

/* ZMAEE IZip 服务对象虚表（IDA 实测 g_aee_zip_vtbl @ .data:0x64574，5 槽 → 0x14）。 */
#define ZIP_VT_ADDR (SHIM_VT_BASE + 0x1B00U)

/* IImage 池虚表（32 槽 → 0x80） */
#define IMAGE_VT_ADDR (SHIM_VT_BASE + 0x2000U)

/* ---- ZMAEE surface 门面虚表（IImage::Decode 的 out 对象）----
 * 对象字段（00000506 运行期确认）：
 *   +0 = vt   +4 = 原始 surface 指针   +8 = 宽   +0xC = 高
 * 关键槽（逆向 sub_388 type1 / sub_4A0）：
 *   +0x10 GetRect(this, out) → 写 int16 矩形 {l,t,r,b}（分派器据此绘制）
 * 其它槽按对象族的常规顺序给安全的空实现/固定值，避免落到"非法外部调用"。
 * 共 21 槽（0x54 字节）。 */
#define SURF_VT_ADDR (SHIM_VT_BASE + 0x2200U)

/* ============================================================
 *                        数 据 区  (0x08000)
 *
 * 非对象、非虚表的零散内存：scratch、上下文、slot。
 * ============================================================ */

/* scratch；顺手把 SIZE_SLOT 从原 0x700 挪开，
 * 解决 DUMMY_BUF / SIZE_SLOT 同址冲突。 */
#define DUMMY_BUF (SHIM_DATA_BASE + 0x000U) /* 0x100 字节 scratch */
#define SIZE_SLOT (SHIM_DATA_BASE + 0x100U) /* applet 写文件大小（待确认） */
#define API_SLOT (SHIM_DATA_BASE + 0x110U)  /* applet 写 API vt */

/* 00000405.app 新增 shim 对象地址：init 事件 r3 上下文（256B 零填充） */
#define INIT_CTX (SHIM_DATA_BASE + 0x200U)

/* ---- create_cbk 的"应用上下文"结构体（00000506 实测）----
 *
 * ROOT_TABLE_ADDR[0x154] create_cbk 返回 CBK_OBJ；applet 随后把
 * **CBK_OBJ+0x48** 当成一个上下文指针来用（getter sub_8884，实测 31
 * 个调用点）：
 *   bl   sub_8884          ; r0 = *(CBK_OBJ + 0x48)
 *   ldr  r0, [r0, #0x50]   ; → IDisplay*（存进资源管理器的 +0x58，
 *                          ;   随后以 vt[0xA8]=CreateImage 调用）
 *   ldr  r0, [r0, #0x54]   ; → 屏幕宽
 *   ldr  r0, [r0, #0x58]   ; → 屏幕高
 * 宽/高的用法是 `add r0,r0,r0,lsr#31; asr r0,r0,#1`（除以 2，配合 ldrsh
 * 取坐标做居中计算），证实是标量尺寸而非对象。
 *
 * 该字段若留空（值为 build_vtables 填的 trap 地址），applet 取到的
 * display 就是野值 → 资源管理器 +0x58 为 0 → 解引用崩溃。
 *
 * CBK_CTX 是**数据区**，必须像 LAYER_BUF 一样排除在 SHIM 的 trap 地址填充
 * 之外并清零。否则 applet 的懒创建逻辑
 *     ldr r0,[ctx,#0x8c]; cmp r0,#0; bne <直接使用>; bl <创建>
 * 会拿到 trap 地址（0x821A8C）当成真实对象解引用 → 崩溃。
 * 未使用字段保持 0，applet 才会走"创建"分支。 */
#define CBK_CTX (SHIM_DATA_BASE + 0x400U)
#define CBK_CTX_SIZE 0x100U

/* ============================================================
 *                        池 区  (0x20000)
 *
 * 多实例对象池：IImage / IBitmap。
 * 每个对象 0x40 字节，各 64 个。
 *
 * RE 依据：IDisplay::CreateImage(alloc, free, &out) 造的 IImage 与
 * IImage::Decode 解出的 IBitmap 在真实固件里是**堆对象**（尺寸/格式随图变），
 * applet 只经虚表使用、不摸字段，因此这里用固定地址池 + 宿主侧记录表实现
 * 多实例（旧实现返回 0/单例，导致 applet 拿到空指针直接崩）。 */
#define IMAGE_POOL (SHIM_POOL_BASE + 0x0000U) /* 512 × 0x40 = 0x8000 */
#define IMAGE_SLOT_SIZE 0x40U
#define IMAGE_SLOT_COUNT 512

#define BITMAP_POOL (SHIM_POOL_BASE + 0x8000U) /* 64 × 0x40 = 0x1000 */
#define BITMAP_SLOT_SIZE 0x40U
#define BITMAP_SLOT_COUNT 64

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
 * 因此池容量按"每张图 2 个槽"规划。 */
#define IMAGE_ENTRY_OFF_SURF 0x08U

/* entry +0x18 = **图像类型字段**（IImage::Decode 的分派键）。
 * RE（ZMAEE_IImage_GetType @0x30490）：该函数没有逻辑，就是
 *     if (this == 0) return -4;
 *     return *(int *)(this + 0x18);
 * 枚举值（ZMAEE_IImage_Decode：`v11 = a1[6]` 后分派）：
 *     0 = GIF（也是对象刚建好时的默认值）
 *     1 = PNG  → IImage_PNG_Decode
 *     2 = JPG  → IImage_JPG_Decode
 *     其它     → 直接 return -1（非法类型）
 * 同一对象上：+0x0C = 原始数据指针，+0x10 = 数据长度（Decode 用它们造
 * IMemStream）。模拟器里 Decode 是整槽 trap，故这两格暂不维护。 */
#define IMAGE_ENTRY_OFF_TYPE 0x18U

/* ============================================================
 *                        像 素 区  (0x30000)
 *
 * 大块像素缓冲，applet 直接读写。
 * 必须清零、绝不填 trap。
 * ============================================================ */
/* ---- 绘制尺寸：真源 = .app 头部解析出的 ScreenW/ScreenH ----
 *
 * 运行期尺寸放在 g_layer_w / g_layer_h（main.c 解析 .app 头部后写入，见
 * parse_app_header 的 ScreenW/ScreenH @0x17C/0x180），它同时决定：
 *   - 层缓冲尺寸（LAYER_BUF / FRAMEBUF / 基础层）
 *   - SDL 窗口尺寸 g_w/g_h（zm_display_init）
 *   - 软件帧缓冲 g_fb_w/g_fb_h
 *   - IShell::GetDeviceInfo 上报的屏幕宽高
 * 三者必须一致，改分辨率只改头部一处。
 *
 * 编译期只保留**容量上限** LAYER_MAX_W/H：静态数组与内存 region 尺寸要用它
 * （如 zm_display.c 的 `static uint16_t prev_fb[...]`）。当前见过的 applet
 * 头部最大 900×900（00000405），故上限取 1024。 */
#define LAYER_MAX_W 1024
#define LAYER_MAX_H 1024

extern int g_layer_w, g_layer_h;
#define LAYER_W g_layer_w
#define LAYER_H g_layer_h

/* ---- 客户机可见的"显示层"像素缓冲 ----
 *
 * applet 通过 IDisplay::GetLayerInfo(disp, 1, &info) 取 layer_info，其中
 *   info[0xC] = 宽、info[0x10] = 高（已实测确认），
 *   info[0x24] = **层像素缓冲指针**（逆向 sub_10248 @0x10318-0x1033C：
 *     surf[0x0]=info[0xC] 宽、surf[0x4]=info[0x10] 高、surf[0x8]=1(格式)、
 *     surf[0x1C]=info[0x24] → 随后把 surf 作为 BitBlt 的源）。
 *
 * 布局为 RGB565、宽高 LAYER_W×LAYER_H；applet 直接读写它，present 时再
 * 转成 ARGB 上传到 SDL 纹理显示。 */
#define LAYER_BUF (SHIM_PIXEL_BASE + 0x000000U)
#define LAYER_BUF_SIZE (LAYER_MAX_W * LAYER_MAX_H * 2) /* 上限容量 2MB */

/* ---- IBitmap 的像素/调色板区 ----
 *
 * RE：ZMAEE_IBitmap_New 里 v5[9] = v5 + 11，即像素指针 = 对象 + 44，
 * 对象与像素是连续分配的一整块。我们是单例对象，无法照搬"对象+44"的
 * 布局（BITMAP 后面 0x80 字节就是 BITMAP_VT_ADDR，放不下像素），
 * 故单独开一块，把 +36 像素指针指过去。
 *
 * 解码像素池（客户机可见）：IImage 解码出的像素必须真落在 guest 内存，
 * 因为 applet 自带的 GDI 是直接按 IBitmap 的 +36 像素指针去读的，
 * 宿主侧那份 rgba 它根本看不到。
 * RE 依据：ZMAEE_IBitmap_GetInfo 就是 `memcpy(out, bitmap + 8, 32)`，
 * 即 out[7] = bitmap[36] = 像素指针。
 * 布局：IBitmap 对象字段 8 个 dword（+8..+40）+ 像素数据。循环复用。 */
#define FRAMEBUF (SHIM_PIXEL_BASE + 0x200000U)
#define FRAMEBUF_SIZE (LAYER_MAX_W * LAYER_MAX_H * 2) /* 上限容量 2MB */

/* 解码像素池（也用于分配"基础层"缓冲，须容纳最大 1024×1024×2 ≈ 2MB）。 */
#define PIX_POOL (SHIM_PIXEL_BASE + 0x400000U)
#define PIX_POOL_SIZE 0x280000U /* 2.5MB */

/* -------------------- trap 地址宏 -------------------- */
#define TRAP(idx) (TRAMP_BASE + (idx) - SHIM_BASE)

#endif /* EMU_MEM_LAYOUT_H */

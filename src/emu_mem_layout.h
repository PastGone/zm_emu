#ifndef EMU_MEM_LAYOUT_H
#define EMU_MEM_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

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
 *   2) 入口 stub（文件偏移 0x188）经 ROOT_TABLE_ADDR 槽写入的 handler =
 * 0x11108C， 它是 applet 自己算出的绝对地址；只有 VA = 文件偏移时该地址才落在
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

#define SHIM_FT_BASE (HEAP_END) // 函数表
#define SHIM_FT_SIZE (HALF_MB / 2)
// 暂时的
#define SHIM_BASE SHIM_FT_BASE
#define SHIM_SIZE (HALF_MB)
//
#define SHIM_OBJ_BASE (HEAP_END)
#define SHIM_OBJ_SIZE (HALF_MB / 2)

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
 * BITMAP_VT_ADDR，放不下像素），故单独开一块，把 +36 像素指针指过去。
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

#define ROOT_TABLE_ADDR (SHIM_BASE + 0x000U)
/* SHELL 必须避开 ROOT_TABLE_ADDR 的函数指针表区。
 * emu.c 把整个 SHIM 按 4 字节步长填成 TRAMP_BASE+i，因此 ROOT_TABLE_ADDR 的表从
 * 0x000 起连续铺开，已知最高槽 +0x154（create_cbk）→ 表区至少
 * 0x000..0x158。SHELL 原先在 0x100、字段延伸到 0x218，正好压住
 * ROOT_TABLE_ADDR 的 0x100..0x158 段（+0x154 create_cbk 落在 SHELL+0x54），
 * 一旦 applet 写 shell 字段就会破坏该槽。现从 0x200 起，给 ROOT_TABLE_ADDR
 * 表留出完整 0x200 字节。 */
#define SHELL (SHIM_BASE + 0x200U)
/* SHELL_VT_ADDR 必须避开 SHELL 的对象字段区。
 * RE 依据（ZMAEE_IShell_ActiveApplet）：IShell 对象是「vptr + 数据字段」，
 * 字段至少到 +0x118（读 a1+276 与 a1+280），故对象区约 0x100..0x220。
 * 虚表原先放在 0x180，正好压在对象 +0x80..+0x108 的字段上——一旦 applet
 * 直接访问 shell 字段（SDK 内联访问器常见）就会重写虚表，与当初 CBK_OBJ
 * 踩碎 DISPLAY vptr 的崩溃同型。现移到 0x240（34 槽 → 0x2C8，不与
 * FileMgr@0x300 冲突）。 */
/* 对象字段区约 0x200..0x31F，虚表独立放 0x400（34 槽 → 0x488，
 * 与 FileMgr@0x500 不冲突） */
#define SHELL_VT_ADDR                                                           \
  (SHIM_BASE + 0x400U) /* g_aee_shell_vtbl @ .data:0x64440，34 槽 */
#define FileMgr (SHIM_BASE + 0x500U)
#define FileMgr_VT_ADDR (SHIM_BASE + 0x580U) /* 16 槽 → 0x5C0 */
#define FILE1 (SHIM_BASE + 0x600U)
#define FILE_VT_ADDR (SHIM_BASE + 0x680U) /* 10 槽 → 0x6A8 */
#define DUMMY_BUF                                                              \
  (SHIM_BASE + 0x700U) /* scratch；至 INIT_CTX@0x800 有 0x100 余量（原 0x750 \
                          头顶仅 0xB0） */
/* 0x100000B ISetting（RE：g_aee_setting_vtbl @ .data:0x64408，14 槽）。
 * 旧名 AUDIO 是误命名：该对象被用于 +0x14 / +0x24，曾按"音频状态"实现；
 * 真实接口是 ISetting（配置读写），音频是下面的 MEDIA。 */
#define SETTING (SHIM_BASE + 0x1800U)
#define SETTING_VT_ADDR (SHIM_BASE + 0x1880U) /* 14 槽 → 0x18B8 */
/* 0x100000C IMedia = 音频（用户确认；RE：g_aee_media_vtbl @ .data:0x640E4，
 * 25 槽）。旧名 AP 是误命名。 */
#define MEDIA (SHIM_BASE + 0x1900U)
#define MEDIA_VT_ADDR (SHIM_BASE + 0x1980U) /* 25 槽 → 0x19E4 */

//

/* 00000405.app 新增 shim 对象地址（0x800 起，与 DUMMY_BUF@0x750 不冲突） */
#define INIT_CTX (SHIM_BASE + 0x800U) /* 256B 零填充：init 事件 r3 上下文 */
/* ---- 服务对象区（每个对象/VT 间隔 0x100+，见下方布局说明）----
 * 布局教训（00001b62 实测）：applet 会把 root.create_cbk 返回的
 * CBK_OBJ 当 ≥0x170 字节的大上下文结构体用（+0x48 存 SHELL、+0x4C 起
 * 存 CreateInstance 服务对象表、+0x128 起填句柄数组）。真实固件里
 * 各对象在 RAM 中相距甚远，互不干扰；此前的紧凑布局（0x900~0xC00
 * 挤 7 个对象）被 applet 上下文写入踩碎 DISPLAY vptr / DISPLAY_VT_ADDR /
 * DLL_OBJ_VT_ADDR，导致读回空指针崩溃。现按每对象 0x800~0x100 间隔拉开。 */
#define NETMGR (SHIM_BASE + 0x900U) /* 0x1000004 INetMgr 服务对象 */
#define NETMGR_VT_ADDR (SHIM_BASE + 0x980U)
#define TAPI (SHIM_BASE + 0xA00U) /* 0x1000009 ITAPI 服务对象 */
#define TAPI_VT_ADDR (SHIM_BASE + 0xA80U)
#define CBK_OBJ                                                                \
  (SHIM_BASE + 0xB00U)                        /* create_cbk 返回对象；    \
                                                 applet 当大上下文用，留 0x800 */
#define CBK_OBJ_VT_ADDR (SHIM_BASE + 0x1300U) /* 可写：applet 覆写 vt[+8] */
#define DLL_OBJ (SHIM_BASE + 0x1400U)         /* loadDLL 返回的 stub DLL 对象 */
#define DLL_OBJ_VT_ADDR (SHIM_BASE + 0x1480U)

/* ---- ZMAEE IDisplay / IBitmap 原生虚表（逆向实测 g_aee_display_vtbl /
 * g_aee_bitmap_vtbl @ .data:0x63E10 / 0x63DF4）----
 * display 是全局单例，由 queryInterface(0x1000005) 返回。旧 GFX/GFX_VT
 * 是早期对同一张表的误命名（实测偏移与本表吻合），已并入此处。
 * bitmap 由 IDisplay.CreateBitmap/LoadBitmap 创建，这里用单个 BITMAP 单例
 * 作为所有 bitmap 对象的 vtable 模板（真实多实例后续再扩展）。 */
/* ---- create_cbk 的"应用上下文"结构体（00000506 实测）----
 *
 * ROOT_TABLE_ADDR[0x154] create_cbk 返回 CBK_OBJ；applet 随后把
 * **CBK_OBJ+0x48** 当成一个上下文指针来用（getter sub_8884，实测 31
 * 个调用点）： bl   sub_8884          ; r0 = *(CBK_OBJ + 0x48) ldr  r0, [r0,
 * #0x50]   ; → IDisplay*（存进资源管理器的 +0x58， ;   随后以
 * vt[0xA8]=CreateImage 调用） ldr  r0, [r0, #0x54]   ; → 屏幕宽 ldr  r0, [r0,
 * #0x58]   ; → 屏幕高 宽/高的用法是 `add r0,r0,r0,lsr#31; asr r0,r0,#1`（除以
 * 2，配合 ldrsh 取坐标做居中计算），证实是标量尺寸而非对象。
 *
 * 该字段若留空（值为 build_vtables 填的 trap 地址），applet 取到的
 * display 就是野值 → 资源管理器 +0x58 为 0 → 解引用崩溃。
 * 地址放在 MEDIA_VT_ADDR(0x1980+25*4=0x19E4) 之后、IMAGE_POOL(0x2000) 之前。 */
#define CBK_CTX (SHIM_BASE + 0x1A00U)
/* CBK_CTX 是**数据区**，必须像 LAYER_BUF 一样排除在 SHIM 的 trap 地址填充
 * 之外并清零。否则 applet 的懒创建逻辑
 *     ldr r0,[ctx,#0x8c]; cmp r0,#0; bne <直接使用>; bl <创建>
 * 会拿到 trap 地址（0x821A8C）当成真实对象解引用 → 崩溃。
 * 未使用字段保持 0，applet 才会走"创建"分支。 */
#define CBK_CTX_SIZE 0x100U

/* IDisplay 对象。需要装下：+0 vptr、+8 活动层索引、+20 每层一个字节的
 * 标志数组、+36 起 16 个 52 字节的层项 —— 合计 ≈ 936 字节。
 * 原先放在 0x1500（到 DISPLAY_VT_ADDR@0x1600 只剩 0x100）根本不够，层项会被
 * 虚表吃掉。现移到 0x1B00..0x1F00（CBK_CTX 之后、IMAGE_POOL 之前）。 */
#define DISPLAY (SHIM_BASE + 0x1B00U)
#define DISPLAY_OBJ_SIZE 0x400U /* 全局 display 对象（0x1000005） */
#define DISPLAY_VT_ADDR (SHIM_BASE + 0x1600U) /* 58 槽 ×4B = 0xE8 */
#define BITMAP                                                                 \
  (SHIM_BASE + 0x1700U) /* bitmap 单例对象（CreateBitmap 旧桩）        \
                         */
#define BITMAP_VT_ADDR (SHIM_BASE + 0x1780U) /* 7 槽 ×4B = 0x1C */

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
#define IUTIL_VT_ADDR (SHIM_BASE + 0x1F80U) /* 7 槽 ×4B = 0x1C */

#define IMAGE_POOL (SHIM_BASE + 0x2000U)    /* 64 × 0x40 = 0x1000 */
#define IMAGE_VT_ADDR (SHIM_BASE + 0x3000U) /* 32 槽 ×4B = 0x80 */
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

/* ---- ZMAEE surface 门面虚表（IImage::Decode 的 out 对象）----
 * 对象字段（00000506 运行期确认）：
 *   +0 = vt   +4 = 原始 surface 指针   +8 = 宽   +0xC = 高
 * 关键槽（逆向 sub_388 type1 / sub_4A0）：
 *   +0x10 GetRect(this, out) → 写 int16 矩形 {l,t,r,b}（分派器据此绘制）
 * 其它槽按对象族的常规顺序给安全的空实现/固定值，避免落到"非法外部调用"。
 * 共 21 槽（0x54 字节）。 */
#define SURF_VT (SHIM_BASE + 0x3200U)

// -------------------- trap 地址宏 --------------------
#define TRAP(idx) (TRAMP_BASE + (idx) - SHIM_BASE)

#endif /* EMU_MEM_LAYOUT_H */

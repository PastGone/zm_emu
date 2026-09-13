#ifndef EMU_MEM_LAYOUT_H
#define EMU_MEM_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * 顶层内存布局
 * ============================================================ */
#define ONE_MB (0x100000U)
#define HALF_MB (0x80000U)

/* payload 在客户机中的映射基址。
 *
 * 关键：applet 的**绝对地址体系就是"文件偏移"**，payload 必须映射到低地址。
 * 证据（00000506）：
 *   1) payload 内的字面量池项是 IDA 标注的相对表达式，例如
 *        off_18D68 DCD loc_188 - 0x18B38   （实际值 0xFFFE7650）
 *      按 VA = 文件偏移 还原得到目标 0x3C0（sub_3A0 的指令），198/198 项全部
 *      落在 payload 范围内；而若按 VA = 偏移 + 0x80000 还原则一项都对不上。
 *   2) 入口 stub（文件偏移 0x188）经 ROOT_TABLE_ADDR 槽写入的 handler =
 *      0x11108C，它是 applet 自己算出的绝对地址；只有 VA = 文件偏移时该地址
 *      才落在 payload 内部（否则读到的是未映射内存里的全 0，执行后 PC 飞出）。
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

#define SHIM_FT_BASE (HEAP_END) /* 函数表 */
#define SHIM_FT_SIZE (HALF_MB / 2)

#define SHIM_BASE SHIM_FT_BASE
#define SHIM_SIZE (1 * ONE_MB)

#define TRAMP_BASE (SHIM_BASE + SHIM_SIZE)
#define TRAMP_SIZE (1 * HALF_MB)

#define ROOT_SLOT_OFF 0x180U
#define APPLET_ENTRY_OFF 0x188U
#define APPLET_ENTRY_POINT (BLOB_BASE + APPLET_ENTRY_OFF)

/* ============================================================
 * SHIM 内部分区
 *
 *   [ VT ][ DATA ][ OBJ ][ POOL ][ PIXEL ][ reserved ]
 *   0   32K    64K   128K   192K   768K       1M
 * ============================================================ */
#define SHIM_VT_BASE (SHIM_BASE + 0x00000U) /* 32KB */
#define SHIM_VT_SIZE (0x08000U)

#define SHIM_DATA_BASE (SHIM_BASE + 0x08000U) /* 32KB */
#define SHIM_DATA_SIZE (0x08000U)

#define SHIM_OBJ_BASE (SHIM_BASE + 0x10000U) /* 64KB */
#define SHIM_OBJ_SIZE (0x10000U)

#define SHIM_POOL_BASE (SHIM_BASE + 0x20000U) /* 64KB */
#define SHIM_POOL_SIZE (0x10000U)

#define SHIM_PIXEL_BASE (SHIM_BASE + 0x30000U) /* 576KB */
#define SHIM_PIXEL_SIZE (0x90000U)

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
 * 真实接口是 ISetting（配置读写），音频是下面的 MEDIA。 */
#define SETTING_VT_ADDR (SHIM_VT_BASE + 0x1800U)

/* 0x100000C IMedia = 音频（RE：g_aee_media_vtbl @ .data:0x640E4，25 槽）。
 * 旧名 AP 是误命名。 */
#define MEDIA_VT_ADDR (SHIM_VT_BASE + 0x1900U)

/* 0x1000013 IUtil 服务对象。真机 util 虚表是 7 个槽（0x00~0x18）。 */
#define IUTIL_VT_ADDR (SHIM_VT_BASE + 0x1A00U)

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
 *                        对 象 区  (0x10000)
 *
 * 各服务对象实例。applet 通过 CreateInstance 返回的指针使用，
 * 不硬编码地址，因此可以整体平移。
 * ============================================================ */

/* IShell 对象。字段至少到 +0x118（RE：a1+276/a1+280）。
 * 必须避开 ROOT_TABLE_ADDR 的函数指针表区（applet stub 可能硬编码
 * SHIM_BASE+0x180 附近的 root slot，G_SHELL_ADDR 不能压住那段）。 */
// 推测的 IShell 全局单例结构体
// 基址：0x65C68
typedef struct obj_shell_st {
  uint32_t *vtable;         // +0x000, 0x65C68, 指向 g_aee_shell_vtbl
  char root_dir[256];       // +0x004, 0x65C6C, 根目录路径缓冲区
  int32_t field_104;        // +0x104, 0x65D6C, 初始 -1，可能是无效句柄
  int32_t ref_count;        // +0x108, 0x65D70, 初始 1，很可能是引用计数
  int32_t field_10C;        // +0x10C, 0x65D74, 初始 0
  int32_t field_110;        // +0x110, 0x65D78, 初始 0
  int32_t applet_related;   // +0x114, 0x65D7C, 被 ZMAEE_GetApplet 引用
  int32_t field_118;        // +0x118, 0x65D80, 初始 -1，句柄/资源 ID
  uint8_t reserved1[0x260]; // +0x11C, 0x65D84 ~ 0x65FE3, 内部扩展区/未知
  int32_t dword_65FE4[5];   // +0x37C, 0x65FE4, 被 CreateInstance 等使用
  int32_t dword_65FF8;      // +0x390, 0x65FF8, 初始 -1
} obj_shell_st;             // 推测大小 0x394 (916 字节)

enum obj_shell_inner_off {
  OBJ_SHELL_OFF_VTABLE = 0x000,         // 0x65C68
  OBJ_SHELL_OFF_ROOT_DIR = 0x004,       // 0x65C6C
  OBJ_SHELL_OFF_FIELD_104 = 0x104,      // 0x65D6C
  OBJ_SHELL_OFF_REF_COUNT = 0x108,      // 0x65D70
  OBJ_SHELL_OFF_FIELD_10C = 0x10C,      // 0x65D74
  OBJ_SHELL_OFF_FIELD_110 = 0x110,      // 0x65D78
  OBJ_SHELL_OFF_APPLET_RELATED = 0x114, // 0x65D7C
  OBJ_SHELL_OFF_FIELD_118 = 0x118,      // 0x65D80
  OBJ_SHELL_OFF_RESERVED1 = 0x11C,      // 0x65D84
  OBJ_SHELL_OFF_DWORD_65FE4 = 0x37C,    // 0x65FE4
  OBJ_SHELL_OFF_DWORD_65FF8 = 0x390,    // 0x65FF8
};

#define G_SHELL_ADDR (SHIM_OBJ_BASE + 0x000U)

/* IFileMgr 对象 */
typedef struct obj_file_mgr_st {
 uint32_t *vtable;
} obj_file_mgr_st;//整个对象加上保留区域占 272 字节,在逆向里。

enum obj_file_mgr_inner_off {
 OBJ_FILE_MGR_OFF_VTABLE = 0x000,
};
#define G_FileMgr_ADDR (SHIM_OBJ_BASE + 0x200U)

#define FILE1 (SHIM_OBJ_BASE + 0x300U)

/* INetMgr / ITAPI 服务对象 */
// 单个 socket 槽位，大小 0x48 (72 字节)
typedef struct netmgr_socket_slot {
 void       *vtable;           // +0x00, 若为 0 则设为 &g_aee_socket_vtbl
 int32_t     field_04;         // +0x04, 未使用
 int32_t     field_08;         // +0x08, 初始化为 -1
 int32_t     field_0C;         // +0x0C, 初始化为 0
 uint8_t     reserved[0x38];   // +0x10 ~ +0x47, 剩余 56 字节
} netmgr_socket_slot;

// NetMgr 全局单例
typedef struct obj_netmgr_st {
 void       *vtable;           // +0x00, 0x64140, 指向 g_aee_netmgr_vtbl
 int32_t     ref_count;        // +0x04, 0x64144, 引用计数
 int32_t     field_08;         // +0x08, 0x64148, 初始 2，代码中设为 1
 int32_t     field_0C;         // +0x0C, 0x6414C, 初始 2，代码中设为 1
 uint8_t     reserved[0x24];   // +0x10 ~ +0x33, 未知，共 36 字节
 netmgr_socket_slot slots[8];  // +0x34, 0x64174 ~ 0x643B3, 共 8 个槽位
 int32_t     field_274;        // +0x274, 0x643B4, 初始 0xFFFFFFFF，循环结束哨兵
} obj_netmgr_st;                  // 总大小 0x278 (632 字节)

enum obj_netmgr_inner_off {
 OBJ_NETMGR_OFF_VTABLE       = 0x00,   // 0x64140
 OBJ_NETMGR_OFF_REF_COUNT    = 0x04,   // 0x64144
 OBJ_NETMGR_OFF_FIELD_08     = 0x08,   // 0x64148
 OBJ_NETMGR_OFF_FIELD_0C     = 0x0C,   // 0x6414C
 OBJ_NETMGR_OFF_RESERVED     = 0x10,   // 0x64150
 OBJ_NETMGR_OFF_SLOTS        = 0x34,   // 0x64174
 OBJ_NETMGR_OFF_FIELD_274    = 0x274,  // 0x643B4
 OBJ_NETMGR_SIZE             = 0x278   // 0x643B8 (g_aee_netmgr_vtbl 起始)
};
enum netmgr_socket_slot_inner_off {
 SOCKET_SLOT_OFF_VTABLE      = 0x00,   // +0x00
 SOCKET_SLOT_OFF_FIELD_04    = 0x04,   // +0x04
 SOCKET_SLOT_OFF_FIELD_08    = 0x08,   // +0x08
 SOCKET_SLOT_OFF_FIELD_0C    = 0x0C,   // +0x0C
 SOCKET_SLOT_OFF_RESERVED    = 0x10,   // +0x10
 SOCKET_SLOT_SIZE            = 0x48    // 72 字节
};

#define G_NETMGR_ADDR (SHIM_OBJ_BASE + 0x400U)
#define TAPI (SHIM_OBJ_BASE + 0x500U)

/* ---- 服务对象区布局教训（00001b62 实测）----
 * applet 会把 root.create_cbk 返回的 CBK_OBJ 当 ≥0x170 字节的大上下文
 * 结构体用（+0x48 存 G_SHELL_ADDR、+0x4C 起存 CreateInstance 服务对象表、
 * +0x128 起填句柄数组）。真实固件里各对象在 RAM 中相距甚远，互不干扰；
 * 此前的紧凑布局被 applet 上下文写入踩碎 DISPLAY vptr / DISPLAY_VT_ADDR /
 * DLL_OBJ_VT_ADDR，导致读回空指针崩溃。现按每对象 0x100+ 间隔拉开。 */
#define CBK_OBJ (SHIM_OBJ_BASE + 0x800U)

/* loadDLL 返回的 stub DLL 对象 */
#define DLL_OBJ (SHIM_OBJ_BASE + 0x1000U)

/* bitmap 单例模板（CreateBitmap 旧桩） */
#define BITMAP (SHIM_OBJ_BASE + 0x1100U)

/* ISetting / IMedia 服务对象 */
#define SETTING (SHIM_OBJ_BASE + 0x1200U)
#define MEDIA (SHIM_OBJ_BASE + 0x1300U)

/* IDisplay 全局单例（0x1000005）。
 * 需要装下：+0 vptr、+0x10 base layer buffer、+0x24 base layer depth、
 * +0x30/+0x34 宽高、+0x364 内嵌子对象 vptr —— 实际至少 0x368 字节，
 * 留 0x400。 */
#define DISPLAY (SHIM_OBJ_BASE + 0x1400U)
#define DISPLAY_OBJ_SIZE 0x400U

/* IUtil 服务对象。实测 applet **每帧**都请求它一次（拿不到就返回 -3
 * 优雅回退）—— 是唯一"每帧都在失败"的服务，因此值得给它一个真实对象。 */
#define IUTIL (SHIM_OBJ_BASE + 0x1800U)

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
#define IMAGE_POOL (SHIM_POOL_BASE + 0x0000U) /* 64 × 0x40 = 0x1000 */
#define IMAGE_SLOT_SIZE 0x40U
#define IMAGE_SLOT_COUNT 64

#define BITMAP_POOL (SHIM_POOL_BASE + 0x2000U) /* 64 × 0x40 = 0x1000 */
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

/* ============================================================
 *                        像 素 区  (0x30000)
 *
 * 大块像素缓冲，applet 直接读写。
 * 必须清零、绝不填 trap。
 * ============================================================ */
#define LAYER_W 240
#define LAYER_H 320

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
#define LAYER_BUF (SHIM_PIXEL_BASE + 0x00000U)
#define LAYER_BUF_SIZE (LAYER_W * LAYER_H * 2) /* 0x25800 = 150KB */

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
#define FRAMEBUF (SHIM_PIXEL_BASE + 0x30000U)
#define FRAMEBUF_SIZE (LAYER_W * LAYER_H * 2) /* 150KB */

/* 解码像素池。 */
#define PIX_POOL (SHIM_PIXEL_BASE + 0x60000U)
#define PIX_POOL_SIZE 0x2A000U /* 168KB */

/* -------------------- trap 地址宏 -------------------- */
#define TRAP(idx) (TRAMP_BASE + (idx) - SHIM_BASE)

#endif /* EMU_MEM_LAYOUT_H */
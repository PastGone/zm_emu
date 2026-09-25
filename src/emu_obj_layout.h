#ifndef EMU_OBJ_LAYOUT_H
#define EMU_OBJ_LAYOUT_H

#include "emu_mem_regions.h"

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

enum obj_shell_inner_off : uint32_t {
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

static constexpr uint32_t G_SHELL_ADDR = SHIM_OBJ_BASE + 0x000U;

/* IFileMgr 对象 */
typedef struct obj_file_mgr_st {
  uint32_t *vtable;
} obj_file_mgr_st; // 整个对象加上保留区域占 272 字节,在逆向里。

enum obj_file_mgr_inner_off : uint32_t {
  OBJ_FILE_MGR_OFF_VTABLE = 0x000,
};
static constexpr uint32_t G_FileMgr_ADDR = SHIM_OBJ_BASE + 0x200U;

typedef struct obj_file_st {
  void *vtable;      // +0x00, 指向 gAEEFileVtbl
  int32_t ref_count; // +0x04, 初始化为 1，很可能是引用计数
  int32_t field_08;  // +0x08, 来自参数 a3
  int32_t field_0C;  // +0x0C, 来自参数 a4
  int32_t field_10;  // +0x10, 初始化为 0
  int32_t field_14;  // +0x14, 初始化为 0
  int32_t field_18;  // +0x18, 初始化为 0
} obj_file_st;       // 总大小 0x1C (28 字节)

enum obj_file_inner_off : uint32_t {
  OBJ_FILE_OFF_VTABLE = 0x00,    // +0x00
  OBJ_FILE_OFF_REF_COUNT = 0x04, // +0x04
  OBJ_FILE_OFF_FIELD_08 = 0x08,  // +0x08
  OBJ_FILE_OFF_FIELD_0C = 0x0C,  // +0x0C
  OBJ_FILE_OFF_FIELD_10 = 0x10,  // +0x10
  OBJ_FILE_OFF_FIELD_14 = 0x14,  // +0x14
  OBJ_FILE_OFF_FIELD_18 = 0x18,  // +0x18
  OBJ_FILE_SIZE = 0x1C           // 28 字节
};

static constexpr uint32_t FILE1 = SHIM_OBJ_BASE + 0x300U;

/* INetMgr / ITAPI 服务对象 */
// 单个 socket 槽位，大小 0x48 (72 字节)
typedef struct netmgr_socket_slot {
  uint32_t *vtable;       // +0x00, 若为 0 则设为 &g_aee_socket_vtbl
  int32_t field_04;       // +0x04, 未使用
  int32_t field_08;       // +0x08, 初始化为 -1
  int32_t field_0C;       // +0x0C, 初始化为 0
  uint8_t reserved[0x38]; // +0x10 ~ +0x47, 剩余 56 字节
} netmgr_socket_slot;

// NetMgr 全局单例
typedef struct obj_netmgr_st {
  uint32_t *vtable;            // +0x00, 0x64140, 指向 g_aee_netmgr_vtbl
  int32_t ref_count;           // +0x04, 0x64144, 引用计数
  int32_t field_08;            // +0x08, 0x64148, 初始 2，代码中设为 1
  int32_t field_0C;            // +0x0C, 0x6414C, 初始 2，代码中设为 1
  uint8_t reserved[0x24];      // +0x10 ~ +0x33, 未知，共 36 字节
  netmgr_socket_slot slots[8]; // +0x34, 0x64174 ~ 0x643B3, 共 8 个槽位
  int32_t field_274;           // +0x274, 0x643B4, 初始 0xFFFFFFFF，循环结束哨兵
} obj_netmgr_st;               // 总大小 0x278 (632 字节)

enum obj_netmgr_inner_off : uint32_t {
  OBJ_NETMGR_OFF_VTABLE = 0x00,     // 0x64140
  OBJ_NETMGR_OFF_REF_COUNT = 0x04,  // 0x64144
  OBJ_NETMGR_OFF_FIELD_08 = 0x08,   // 0x64148
  OBJ_NETMGR_OFF_FIELD_0C = 0x0C,   // 0x6414C
  OBJ_NETMGR_OFF_RESERVED = 0x10,   // 0x64150
  OBJ_NETMGR_OFF_SLOTS = 0x34,      // 0x64174
  OBJ_NETMGR_OFF_FIELD_274 = 0x274, // 0x643B4
  OBJ_NETMGR_SIZE = 0x278           // 0x643B8 (g_aee_netmgr_vtbl 起始)
};
enum netmgr_socket_slot_inner_off : uint32_t {
  SOCKET_SLOT_OFF_VTABLE = 0x00,   // +0x00
  SOCKET_SLOT_OFF_FIELD_04 = 0x04, // +0x04
  SOCKET_SLOT_OFF_FIELD_08 = 0x08, // +0x08
  SOCKET_SLOT_OFF_FIELD_0C = 0x0C, // +0x0C
  SOCKET_SLOT_OFF_RESERVED = 0x10, // +0x10
  SOCKET_SLOT_SIZE = 0x48          // 72 字节
};

static constexpr uint32_t G_NETMGR_ADDR = SHIM_OBJ_BASE + 0x400U;
typedef struct obj_itapi_st {
  void *vtable;           // +0x00, 0x6677C, 指向 g_aee_tapi_vtbl
  int32_t field_04;       // +0x04, 0x66780, 初始化为 1
  uint8_t reserved[0x88]; // +0x08 ~ +0x8F, 0x66784 ~ 0x6680B, 零初始化区
  void *field_90;         // +0x90, 0x6680C, off_6680C, 被 ITAPI 方法引用
  int32_t field_94;       // +0x94, 0x66810, 被 ITAPI 方法读写
} obj_itapi_st;           // 总大小 0x98 (152 字节)

enum obj_itapi_inner_off : uint32_t {
  OBJ_ITAPI_OFF_VTABLE = 0x00,   // 0x6677C
  OBJ_ITAPI_OFF_FIELD_04 = 0x04, // 0x66780
  OBJ_ITAPI_OFF_RESERVED = 0x08, // 0x66784
  OBJ_ITAPI_OFF_FIELD_90 = 0x90, // 0x6680C
  OBJ_ITAPI_OFF_FIELD_94 = 0x94, // 0x66810
};

static constexpr uint32_t G_TAPI_ADDR = SHIM_OBJ_BASE + 0x500U;

/* ---- 服务对象区布局教训（00001b62 实测）----
 * applet 会把 root.create_cbk 返回的 CBK_OBJ 当 ≥0x170 字节的大上下文
 * 结构体用（+0x48 存 G_SHELL_ADDR、+0x4C 起存 CreateInstance 服务对象表、
 * +0x128 起填句柄数组）。真实固件里各对象在 RAM 中相距甚远，互不干扰；
 * 此前的紧凑布局被 applet 上下文写入踩碎 DISPLAY vptr / DISPLAY_VT_ADDR /
 * DLL_OBJ_VT_ADDR，导致读回空指针崩溃。现按每对象 0x100+ 间隔拉开。 */
static constexpr uint32_t CBK_OBJ = SHIM_OBJ_BASE + 0x800U;

/* loadDLL 返回的 stub DLL 对象 */
static constexpr uint32_t DLL_OBJ = SHIM_OBJ_BASE + 0x1000U;

/* bitmap 单例模板（CreateBitmap 旧桩） */
// 固定头部 44 字节，后面紧跟 a3 字节的额外数据
typedef struct obj_bitmap_st {
  void *vtable;      // +0x00, 指向 g_aee_bitmap_vtbl
  int32_t ref_count; // +0x04, 初始化为 1，很可能是引用计数
  int32_t field_08;  // +0x08, 初始化为 0
  int32_t field_0C;  // +0x0C, 初始化为 0
  int32_t field_10;  // +0x10, 初始化为 0
  int32_t field_14;  // +0x14, 初始化为 0
  int32_t field_18;  // +0x18, 初始化为 0
  int32_t field_1C;  // +0x1C, 初始化为 0
  int32_t field_20;  // +0x20, 初始化为 0
  void *data_ptr;    // +0x24, 指向附加数据区（v5 + 11）
  int32_t field_28;  // +0x28, 初始化为 0
                     // 后面是 a3 字节的额外数据
} obj_bitmap_st;     // 固定头部大小 0x2C (44 字节)
enum obj_bitmap_inner_off : uint32_t {
  OBJ_BITMAP_OFF_VTABLE = 0x00,    // +0x00
  OBJ_BITMAP_OFF_REF_COUNT = 0x04, // +0x04
  OBJ_BITMAP_OFF_FIELD_08 = 0x08,  // +0x08
  OBJ_BITMAP_OFF_FIELD_0C = 0x0C,  // +0x0C
  OBJ_BITMAP_OFF_FIELD_10 = 0x10,  // +0x10
  OBJ_BITMAP_OFF_FIELD_14 = 0x14,  // +0x14
  OBJ_BITMAP_OFF_FIELD_18 = 0x18,  // +0x18
  OBJ_BITMAP_OFF_FIELD_1C = 0x1C,  // +0x1C
  OBJ_BITMAP_OFF_FIELD_20 = 0x20,  // +0x20
  OBJ_BITMAP_OFF_DATA_PTR = 0x24,  // +0x24, 指向附加数据
  OBJ_BITMAP_OFF_FIELD_28 = 0x28,  // +0x28
  OBJ_BITMAP_HEADER_SIZE = 0x2C    // 固定头部 44 字节
};
/* 原名 BITMAP，与 Windows <wingdi.h> 的 `typedef struct tagBITMAP BITMAP;`
 * 撞名（SDL 会间接包含 windows.h），MSVC/clang-cl 下直接报
 * "redefinition of 'BITMAP' as different kind of symbol"。按本文件
 * G_SHELL_ADDR / G_FileMgr_ADDR 的惯例改名为 G_BITMAP_ADDR。 */
static constexpr uint32_t G_BITMAP_ADDR = SHIM_OBJ_BASE + 0x1100U;

/* ISetting / IMedia 服务对象 */
typedef struct obj_setting_st {
  void *vtable;      // +0x00, 指向 g_aee_setting_vtbl
  int32_t ref_count; // +0x04, 初始化为 1，很可能是引用计数
  int32_t field_08;  // +0x08, 初始化为 0
  int32_t field_0C;  // +0x0C, 初始化为 0
  int32_t field_10;  // +0x10, 初始化为 0
} obj_setting_st;    // 总大小 0x14 (20 字节)

enum obj_setting_inner_off : uint32_t {
  OBJ_SETTING_OFF_VTABLE = 0x00,    // +0x00
  OBJ_SETTING_OFF_REF_COUNT = 0x04, // +0x04
  OBJ_SETTING_OFF_FIELD_08 = 0x08,  // +0x08
  OBJ_SETTING_OFF_FIELD_0C = 0x0C,  // +0x0C
  OBJ_SETTING_OFF_FIELD_10 = 0x10,  // +0x10
  OBJ_SETTING_SIZE = 0x14           // 20 字节
};

static constexpr uint32_t SETTING = SHIM_OBJ_BASE + 0x1200U;
//
typedef struct obj_media_st {
  void *vtable;     // +0x00, 指向 g_aee_media_vtbl
  int32_t field_04; // +0x04, 每次调用设为 1
  int32_t field_08; // +0x08, 初始化为 0
  int32_t field_0C; // +0x0C, 初始化为 0
  int32_t field_10; // +0x10, 初始化为 0
  int32_t field_14; // +0x14, 初始化为 0
  int32_t field_18; // +0x18, 初始化为 0
  int32_t field_1C; // +0x1C, 初始化为 0
  int32_t field_20; // +0x20, 初始化为 0
  int32_t field_24; // +0x24, 初始化为 0
  int32_t field_28; // +0x28, 初始化为 0
  int32_t field_2C; // +0x2C, 初始化为 0
} obj_media_st;     // 总大小 0x30 (48 字节)

enum obj_media_inner_off : uint32_t {
  OBJ_MEDIA_OFF_VTABLE = 0x00,   // +0x00
  OBJ_MEDIA_OFF_FIELD_04 = 0x04, // +0x04
  OBJ_MEDIA_OFF_FIELD_08 = 0x08, // +0x08
  OBJ_MEDIA_OFF_FIELD_0C = 0x0C, // +0x0C
  OBJ_MEDIA_OFF_FIELD_10 = 0x10, // +0x10
  OBJ_MEDIA_OFF_FIELD_14 = 0x14, // +0x14
  OBJ_MEDIA_OFF_FIELD_18 = 0x18, // +0x18
  OBJ_MEDIA_OFF_FIELD_1C = 0x1C, // +0x1C
  OBJ_MEDIA_OFF_FIELD_20 = 0x20, // +0x20
  OBJ_MEDIA_OFF_FIELD_24 = 0x24, // +0x24
  OBJ_MEDIA_OFF_FIELD_28 = 0x28, // +0x28
  OBJ_MEDIA_OFF_FIELD_2C = 0x2C, // +0x2C
  OBJ_MEDIA_SIZE = 0x30          // 48 字节
};

static constexpr uint32_t G_MEDIA_ADDR = SHIM_OBJ_BASE + 0x1300U;

/* IDisplay 全局单例（0x1000005）。
 * 需要装下：+0 vptr、+0x10 base layer buffer、+0x24 base layer depth、
 * +0x30/+0x34 宽高、+0x364 内嵌子对象 vptr —— 实际至少 0x368 字节，
 * 留 0x400。 */
typedef struct obj_idisplay_st {
  void *vtable;             // +0x00, 0x64C04, 指向 off_63E10
  int32_t field_04;         // +0x04, 0x64C08, 初始化为 1
  uint8_t reserved1[0x08];  // +0x08 ~ +0x0F, 未在初始化中赋值
  int32_t base_layer_buf;   // +0x10, 0x64C14, 基础层缓冲区
  uint8_t reserved2[0x10];  // +0x14 ~ +0x23, 未赋值
  int32_t base_layer_depth; // +0x24, 0x64C28, 基础层深度
  int32_t field_28;         // +0x28, 0x64C2C, 初始化为 0
  int32_t field_2C;         // +0x2C, 0x64C30, 初始化为 0
  int32_t field_30;         // +0x30, 0x64C34, 来自 v8
  int32_t field_34;         // +0x34, 0x64C38, 来自 v7
  int32_t field_38;         // +0x38, 0x64C3C, 初始化为 0
  int32_t field_3C;         // +0x3C, 0x64C40, 初始化为 0
  int32_t field_40;         // +0x40, 0x64C44, 再次保存 v8
  int32_t field_44;         // +0x44, 0x64C48, 再次保存 v7
  int32_t field_48;         // +0x48, 0x64C4C, 再次保存 base_layer_buf
} obj_idisplay_st;          // 推测大小至少 0x4C (76 字节)

enum obj_idisplay_inner_off : uint32_t {
  OBJ_IDISPLAY_OFF_VTABLE = 0x00,           // 0x64C04
  OBJ_IDISPLAY_OFF_FIELD_04 = 0x04,         // 0x64C08
  OBJ_IDISPLAY_OFF_RESERVED1 = 0x08,        // 0x64C0C
  OBJ_IDISPLAY_OFF_BASE_LAYER_BUF = 0x10,   // 0x64C14
  OBJ_IDISPLAY_OFF_RESERVED2 = 0x14,        // 0x64C18
  OBJ_IDISPLAY_OFF_BASE_LAYER_DEPTH = 0x24, // 0x64C28
  OBJ_IDISPLAY_OFF_FIELD_28 = 0x28,         // 0x64C2C
  OBJ_IDISPLAY_OFF_FIELD_2C = 0x2C,         // 0x64C30
  OBJ_IDISPLAY_OFF_FIELD_30 = 0x30,         // 0x64C34
  OBJ_IDISPLAY_OFF_FIELD_34 = 0x34,         // 0x64C38
  OBJ_IDISPLAY_OFF_FIELD_38 = 0x38,         // 0x64C3C
  OBJ_IDISPLAY_OFF_FIELD_3C = 0x3C,         // 0x64C40
  OBJ_IDISPLAY_OFF_FIELD_40 = 0x40,         // 0x64C44
  OBJ_IDISPLAY_OFF_FIELD_44 = 0x44,         // 0x64C48
  OBJ_IDISPLAY_OFF_FIELD_48 = 0x48,         // 0x64C4C
  OBJ_IDISPLAY_SIZE = 0x4C                  // 至少 76 字节，可能更大
};

static constexpr uint32_t DISPLAY = SHIM_OBJ_BASE + 0x1400U;
static constexpr uint32_t DISPLAY_OBJ_SIZE = 0x400U;

/* IUtil 服务对象。实测 applet **每帧**都请求它一次（拿不到就返回 -3
 * 优雅回退）—— 是唯一"每帧都在失败"的服务，因此值得给它一个真实对象。 */
typedef struct obj_iutil_st {
  void *vtable;     // +0x00, 0x66814, 指向 g_aee_util_vtbl
  int32_t field_04; // +0x04, 0x66818, 初始化为 1
                    // 后面未知，可能没有更多字段，也可能有但未被引用
} obj_iutil_st;     // 已知大小至少 0x08 (8 字节)

enum obj_iutil_inner_off : uint32_t {
  OBJ_IUTIL_OFF_VTABLE = 0x00,   // 0x66814
  OBJ_IUTIL_OFF_FIELD_04 = 0x04, // 0x66818
  OBJ_IUTIL_SIZE = 0x08          // 至少 8 字节
};

static constexpr uint32_t G_IUTIL_ADDR = SHIM_OBJ_BASE + 0x1800U;

/* ZMAEE IZip 服务对象（0x100000F）。
 * 仅返回模拟对象地址，其 +0 vtable 指向 ZIP_VT_ADDR，方法走 zm_zip_stub
 * 观测探针。 */

typedef struct obj_zip_st {
  void *vtable;      // +0x00, 指向 g_aee_zip_vtbl
  int32_t ref_count; // +0x04, 初始化为 1，很可能是引用计数
  int32_t field_08;  // +0x08, 0
  int32_t field_0C;  // +0x0C, 0
  int32_t field_10;  // +0x10, 0
  int32_t field_14;  // +0x14, 初始化为 -1
  int32_t field_18;  // +0x18, 0
  int32_t field_1C;  // +0x1C, 0
  int32_t field_20;  // +0x20, 0
  int32_t field_24;  // +0x24, 0
  int32_t field_28;  // +0x28, 0
  int32_t field_2C;  // +0x2C, 0
  int32_t field_30;  // +0x30, 0
  int32_t field_34;  // +0x34, 0
  int32_t field_38;  // +0x38, 0
} obj_zip_st;        // 总大小 0x3C (60 字节)

enum obj_zip_inner_off : uint32_t {
  OBJ_ZIP_OFF_VTABLE = 0x00,    // +0x00
  OBJ_ZIP_OFF_REF_COUNT = 0x04, // +0x04
  OBJ_ZIP_OFF_FIELD_08 = 0x08,  // +0x08
  OBJ_ZIP_OFF_FIELD_0C = 0x0C,  // +0x0C
  OBJ_ZIP_OFF_FIELD_10 = 0x10,  // +0x10
  OBJ_ZIP_OFF_FIELD_14 = 0x14,  // +0x14
  OBJ_ZIP_OFF_FIELD_18 = 0x18,  // +0x18
  OBJ_ZIP_OFF_FIELD_1C = 0x1C,  // +0x1C
  OBJ_ZIP_OFF_FIELD_20 = 0x20,  // +0x20
  OBJ_ZIP_OFF_FIELD_24 = 0x24,  // +0x24
  OBJ_ZIP_OFF_FIELD_28 = 0x28,  // +0x28
  OBJ_ZIP_OFF_FIELD_2C = 0x2C,  // +0x2C
  OBJ_ZIP_OFF_FIELD_30 = 0x30,  // +0x30
  OBJ_ZIP_OFF_FIELD_34 = 0x34,  // +0x34
  OBJ_ZIP_OFF_FIELD_38 = 0x38,  // +0x38
  OBJ_ZIP_SIZE = 0x3C           // 60 字节
};

static constexpr uint32_t ZIP_ADDR = SHIM_OBJ_BASE + 0x1900U;

/* 以下为"尚未实现"的服务对象占位地址（CreateInstance 0x1000006 IGps /
 * 0x1000007 IGSensor / 0x100000A IAddrBook / 0x100000E IMemStream /
 * 0x1000010 IStatusBar）。其 +0 不单独覆盖，由 build_vtables 默认填成 trap
 * 地址，applet 经对象调方法会落到 trap default（观测、不崩），待 RE 后补实现。
 */
typedef struct obj_gps_st {
  void *vtable;      // +0x00, 指向 g_aee_gps_vtbl
  int32_t ref_count; // +0x04, 初始化为 1，很可能是引用计数
} obj_gps_st;        // 总大小 0x08 (8 字节)

enum obj_gps_inner_off : uint32_t {
  OBJ_GPS_OFF_VTABLE = 0x00,    // +0x00
  OBJ_GPS_OFF_REF_COUNT = 0x04, // +0x04
  OBJ_GPS_SIZE = 0x08           // 8 字节
};

static constexpr uint32_t G_GPS_ADDR = SHIM_OBJ_BASE + 0x1A00U;
typedef struct obj_gsensor_st {
  void *vtable;      // +0x00, 指向 g_aee_gsensor_vtbl
  int32_t ref_count; // +0x04, 初始化为 1，很可能是引用计数
} obj_gsensor_st;    // 总大小 0x08 (8 字节)

enum obj_gsensor_inner_off : uint32_t {
  OBJ_GSENSOR_OFF_VTABLE = 0x00,    // +0x00
  OBJ_GSENSOR_OFF_REF_COUNT = 0x04, // +0x04
  OBJ_GSENSOR_SIZE = 0x08           // 8 字节
};

static constexpr uint32_t G_GSENSOR_ADDR = SHIM_OBJ_BASE + 0x1B00U;
typedef struct obj_addrbook_st {
  void *vtable;            // +0x00, 指向 g_aee_addrbook_vtbl
  int32_t ref_count;       // +0x04, 初始化为 1，很可能是引用计数
  uint8_t reserved[0x108]; // +0x08 ~ +0x10F, 全部清零，未知
} obj_addrbook_st;         // 总大小 0x110 (272 字节)

enum obj_addrbook_inner_off : uint32_t {
  OBJ_ADDRBOOK_OFF_VTABLE = 0x00,    // +0x00
  OBJ_ADDRBOOK_OFF_REF_COUNT = 0x04, // +0x04
  OBJ_ADDRBOOK_OFF_RESERVED = 0x08,  // +0x08
  OBJ_ADDRBOOK_SIZE = 0x110          // 272 字节
};

static constexpr uint32_t G_ADDRBOOK_ADDR = SHIM_OBJ_BASE + 0x1C00U;
typedef struct obj_memstream_st {
  void *vtable;     // +0x00, 指向 g_aee_memstream_vtbl
  int32_t field_04; // +0x04, 未初始化（malloc 后未赋值）
  int32_t field_08; // +0x08, 初始化为 1
  int32_t field_0C; // +0x0C, 初始化为 0
  int32_t field_10; // +0x10, 初始化为 0
  int32_t field_14; // +0x14, 初始化为 0
} obj_memstream_st; // 总大小 0x18 (24 字节)

enum obj_memstream_inner_off : uint32_t {
  OBJ_MEMSTREAM_OFF_VTABLE = 0x00,   // +0x00
  OBJ_MEMSTREAM_OFF_FIELD_04 = 0x04, // +0x04
  OBJ_MEMSTREAM_OFF_FIELD_08 = 0x08, // +0x08
  OBJ_MEMSTREAM_OFF_FIELD_0C = 0x0C, // +0x0C
  OBJ_MEMSTREAM_OFF_FIELD_10 = 0x10, // +0x10
  OBJ_MEMSTREAM_OFF_FIELD_14 = 0x14, // +0x14
  OBJ_MEMSTREAM_SIZE = 0x18          // 24 字节
};
static constexpr uint32_t G_MEMSTREAM_ADDR = SHIM_OBJ_BASE + 0x1D00U;
typedef struct obj_statusbar_st {
  void *vtable;      // +0x00, 指向 g_aee_statusbar_vtbl
  int32_t ref_count; // +0x04, 初始化为 1，很可能是引用计数
} obj_statusbar_st;  // 总大小 0x08 (8 字节)

enum obj_statusbar_inner_off : uint32_t {
  OBJ_STATUSBAR_OFF_VTABLE = 0x00,    // +0x00
  OBJ_STATUSBAR_OFF_REF_COUNT = 0x04, // +0x04
  OBJ_STATUSBAR_SIZE = 0x08           // 8 字节
};

static constexpr uint32_t G_STATUSBAR_ADDR = SHIM_OBJ_BASE + 0x1E00U;

// 全局数组，最多 16 个 HTTP 对象指针
extern int *dword_659C8[16];

typedef struct obj_http_st {
  void *vtable;         // +0x00, 指向 g_aee_http_vtbl
  int32_t ref_count;    // +0x04, 初始化为 1，很可能是引用计数
  int32_t field_08;     // +0x08, 来自参数 a4
  int32_t field_0C;     // +0x0C, 未初始化（malloc 后未赋值）
  int32_t field_10;     // +0x10, 来自参数 a3
  int32_t field_14;     // +0x14, 初始化为 0
  int32_t field_18;     // +0x18, 初始化为 0
  uint8_t buffer[1024]; // +0x1C ~ +0x41B, 被 memset 清零，可能是请求/响应缓冲区
  int32_t field_41C;    // +0x41C, v10[263], 初始化为 0
  int32_t field_420;    // +0x420, v10[264], 初始化为 0
  int32_t field_424;    // +0x424, v10[265], 初始化为 0
  int32_t field_428;    // +0x428, v10[266], 初始化为 0
  int32_t field_42C;    // +0x42C, v10[267], 未赋值
  int32_t field_430;    // +0x430, v10[268], 初始化为 0
  int32_t field_434;    // +0x434, v10[269], 未赋值
  int32_t field_438;    // +0x438, v10[270], 未赋值
  int32_t field_43C;    // +0x43C, v10[271], 未赋值
  int32_t field_440;    // +0x440, v10[272], 未赋值
  int32_t field_444;    // +0x444, v10[273], 未赋值
  int32_t slot_index;   // +0x448, v10[274], 在全局数组中的槽位索引 0~15
  int32_t field_44C;    // +0x44C, v10[275], 初始化为 1
} obj_http_st;          // 总大小 0x450 (1104 字节)

enum obj_http_inner_off : uint32_t {
  OBJ_HTTP_OFF_VTABLE = 0x00,      // +0x00
  OBJ_HTTP_OFF_REF_COUNT = 0x04,   // +0x04
  OBJ_HTTP_OFF_FIELD_08 = 0x08,    // +0x08
  OBJ_HTTP_OFF_FIELD_0C = 0x0C,    // +0x0C
  OBJ_HTTP_OFF_FIELD_10 = 0x10,    // +0x10
  OBJ_HTTP_OFF_FIELD_14 = 0x14,    // +0x14
  OBJ_HTTP_OFF_FIELD_18 = 0x18,    // +0x18
  OBJ_HTTP_OFF_BUFFER = 0x1C,      // +0x1C, 1024 字节
  OBJ_HTTP_OFF_FIELD_41C = 0x41C,  // +0x41C
  OBJ_HTTP_OFF_FIELD_420 = 0x420,  // +0x420
  OBJ_HTTP_OFF_FIELD_424 = 0x424,  // +0x424
  OBJ_HTTP_OFF_FIELD_428 = 0x428,  // +0x428
  OBJ_HTTP_OFF_FIELD_42C = 0x42C,  // +0x42C
  OBJ_HTTP_OFF_FIELD_430 = 0x430,  // +0x430
  OBJ_HTTP_OFF_FIELD_434 = 0x434,  // +0x434
  OBJ_HTTP_OFF_FIELD_438 = 0x438,  // +0x438
  OBJ_HTTP_OFF_FIELD_43C = 0x43C,  // +0x43C
  OBJ_HTTP_OFF_FIELD_440 = 0x440,  // +0x440
  OBJ_HTTP_OFF_FIELD_444 = 0x444,  // +0x444
  OBJ_HTTP_OFF_SLOT_INDEX = 0x448, // +0x448
  OBJ_HTTP_OFF_FIELD_44C = 0x44C,  // +0x44C
  OBJ_HTTP_SIZE = 0x450            // 1104 字节
};

// 全局数组
static constexpr uint32_t HTTP_MAX_INSTANCES = 16;

//

typedef struct idisplay_layer_st {
  uint8_t reserved0[0x24]; // +0x00 ~ +0x23
  int32_t format;          // +0x24
  int32_t field_28;        // +0x28
  int32_t field_2C;        // +0x2C
  int32_t width;           // +0x30
  int32_t height;          // +0x34
  int32_t field_38;        // +0x38
  int32_t field_3C;        // +0x3C
  int32_t width2;          // +0x40
  int32_t height2;         // +0x44
  void *buffer;            // +0x48
  uint8_t reserved1[0x0C]; // +0x4C ~ +0x57
} idisplay_layer_st;       // 至少 0x58 (88 字节)

enum idisplay_layer_inner_off : uint32_t {
  IDISPLAY_LAYER_OFF_FORMAT = 0x24,
  IDISPLAY_LAYER_OFF_FIELD_28 = 0x28,
  IDISPLAY_LAYER_OFF_FIELD_2C = 0x2C,
  IDISPLAY_LAYER_OFF_WIDTH = 0x30,
  IDISPLAY_LAYER_OFF_HEIGHT = 0x34,
  IDISPLAY_LAYER_OFF_FIELD_38 = 0x38,
  IDISPLAY_LAYER_OFF_FIELD_3C = 0x3C,
  IDISPLAY_LAYER_OFF_WIDTH2 = 0x40,
  IDISPLAY_LAYER_OFF_HEIGHT2 = 0x44,
  IDISPLAY_LAYER_OFF_BUFFER = 0x48,
  IDISPLAY_LAYER_SIZE = 0x58,
  IDISPLAY_LAYER_STRIDE_CODE = 0x34 // 代码中的步长，与大小矛盾
};

#endif /* EMU_OBJ_LAYOUT_H */

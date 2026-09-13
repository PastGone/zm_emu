#include "emu_mem_regions.h" /* 拿 SHIM_OBJ_BASE */

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
} obj_file_mgr_st; // 整个对象加上保留区域占 272 字节,在逆向里。

enum obj_file_mgr_inner_off {
  OBJ_FILE_MGR_OFF_VTABLE = 0x000,
};
#define G_FileMgr_ADDR (SHIM_OBJ_BASE + 0x200U)

typedef struct obj_file_st {
  void *vtable;      // +0x00, 指向 gAEEFileVtbl
  int32_t ref_count; // +0x04, 初始化为 1，很可能是引用计数
  int32_t field_08;  // +0x08, 来自参数 a3
  int32_t field_0C;  // +0x0C, 来自参数 a4
  int32_t field_10;  // +0x10, 初始化为 0
  int32_t field_14;  // +0x14, 初始化为 0
  int32_t field_18;  // +0x18, 初始化为 0
} obj_file_st;       // 总大小 0x1C (28 字节)

enum obj_file_inner_off {
  OBJ_FILE_OFF_VTABLE = 0x00,    // +0x00
  OBJ_FILE_OFF_REF_COUNT = 0x04, // +0x04
  OBJ_FILE_OFF_FIELD_08 = 0x08,  // +0x08
  OBJ_FILE_OFF_FIELD_0C = 0x0C,  // +0x0C
  OBJ_FILE_OFF_FIELD_10 = 0x10,  // +0x10
  OBJ_FILE_OFF_FIELD_14 = 0x14,  // +0x14
  OBJ_FILE_OFF_FIELD_18 = 0x18,  // +0x18
  OBJ_FILE_SIZE = 0x1C           // 28 字节
};

#define FILE1 (SHIM_OBJ_BASE + 0x300U)

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

enum obj_netmgr_inner_off {
  OBJ_NETMGR_OFF_VTABLE = 0x00,     // 0x64140
  OBJ_NETMGR_OFF_REF_COUNT = 0x04,  // 0x64144
  OBJ_NETMGR_OFF_FIELD_08 = 0x08,   // 0x64148
  OBJ_NETMGR_OFF_FIELD_0C = 0x0C,   // 0x6414C
  OBJ_NETMGR_OFF_RESERVED = 0x10,   // 0x64150
  OBJ_NETMGR_OFF_SLOTS = 0x34,      // 0x64174
  OBJ_NETMGR_OFF_FIELD_274 = 0x274, // 0x643B4
  OBJ_NETMGR_SIZE = 0x278           // 0x643B8 (g_aee_netmgr_vtbl 起始)
};
enum netmgr_socket_slot_inner_off {
  SOCKET_SLOT_OFF_VTABLE = 0x00,   // +0x00
  SOCKET_SLOT_OFF_FIELD_04 = 0x04, // +0x04
  SOCKET_SLOT_OFF_FIELD_08 = 0x08, // +0x08
  SOCKET_SLOT_OFF_FIELD_0C = 0x0C, // +0x0C
  SOCKET_SLOT_OFF_RESERVED = 0x10, // +0x10
  SOCKET_SLOT_SIZE = 0x48          // 72 字节
};

#define G_NETMGR_ADDR (SHIM_OBJ_BASE + 0x400U)
typedef struct obj_itapi_st {
  void *vtable;           // +0x00, 0x6677C, 指向 g_aee_tapi_vtbl
  int32_t field_04;       // +0x04, 0x66780, 初始化为 1
  uint8_t reserved[0x88]; // +0x08 ~ +0x8F, 0x66784 ~ 0x6680B, 零初始化区
  void *field_90;         // +0x90, 0x6680C, off_6680C, 被 ITAPI 方法引用
  int32_t field_94;       // +0x94, 0x66810, 被 ITAPI 方法读写
} obj_itapi_st;           // 总大小 0x98 (152 字节)

enum obj_itapi_inner_off {
  OBJ_ITAPI_OFF_VTABLE = 0x00,   // 0x6677C
  OBJ_ITAPI_OFF_FIELD_04 = 0x04, // 0x66780
  OBJ_ITAPI_OFF_RESERVED = 0x08, // 0x66784
  OBJ_ITAPI_OFF_FIELD_90 = 0x90, // 0x6680C
  OBJ_ITAPI_OFF_FIELD_94 = 0x94, // 0x66810
};

#define G_TAPI_ADDR (SHIM_OBJ_BASE + 0x500U)

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
enum obj_bitmap_inner_off {
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
#define BITMAP (SHIM_OBJ_BASE + 0x1100U)

/* ISetting / IMedia 服务对象 */
typedef struct obj_setting_st {
  void *vtable;      // +0x00, 指向 g_aee_setting_vtbl
  int32_t ref_count; // +0x04, 初始化为 1，很可能是引用计数
  int32_t field_08;  // +0x08, 初始化为 0
  int32_t field_0C;  // +0x0C, 初始化为 0
  int32_t field_10;  // +0x10, 初始化为 0
} obj_setting_st;    // 总大小 0x14 (20 字节)

enum obj_setting_inner_off {
  OBJ_SETTING_OFF_VTABLE = 0x00,    // +0x00
  OBJ_SETTING_OFF_REF_COUNT = 0x04, // +0x04
  OBJ_SETTING_OFF_FIELD_08 = 0x08,  // +0x08
  OBJ_SETTING_OFF_FIELD_0C = 0x0C,  // +0x0C
  OBJ_SETTING_OFF_FIELD_10 = 0x10,  // +0x10
  OBJ_SETTING_SIZE = 0x14           // 20 字节
};

#define SETTING (SHIM_OBJ_BASE + 0x1200U)
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

enum obj_media_inner_off {
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

#define G_MEDIA_ADDR (SHIM_OBJ_BASE + 0x1300U)

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

enum obj_idisplay_inner_off {
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

#define DISPLAY (SHIM_OBJ_BASE + 0x1400U)
#define DISPLAY_OBJ_SIZE 0x400U

/* IUtil 服务对象。实测 applet **每帧**都请求它一次（拿不到就返回 -3
 * 优雅回退）—— 是唯一"每帧都在失败"的服务，因此值得给它一个真实对象。 */
typedef struct obj_iutil_st {
  void *vtable;     // +0x00, 0x66814, 指向 g_aee_util_vtbl
  int32_t field_04; // +0x04, 0x66818, 初始化为 1
                    // 后面未知，可能没有更多字段，也可能有但未被引用
} obj_iutil_st;     // 已知大小至少 0x08 (8 字节)

enum obj_iutil_inner_off {
  OBJ_IUTIL_OFF_VTABLE = 0x00,   // 0x66814
  OBJ_IUTIL_OFF_FIELD_04 = 0x04, // 0x66818
  OBJ_IUTIL_SIZE = 0x08          // 至少 8 字节
};

#define G_IUTIL_ADDR (SHIM_OBJ_BASE + 0x1800U)

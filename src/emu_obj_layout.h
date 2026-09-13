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

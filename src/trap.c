#include "./trap.h"
#include "hook.h" /* hook_ctx_apply：applet 上下文镜像 */
#include "./log/log.h"
#include "./test/zm_stat.h" /* zm_stat_trap：槽位调用次数统计（ZM_STAT=1） */
#include <inttypes.h> /* PRIx32，用于第 294 行格式化输出 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include "./emu.h"
#include "./tool/odds.h"
#include "./tool/uc_helper.h" /* uc_read32/uc_write32（读栈上参数 a5） */
#include "./ulibc/ulibc.h" /* 客户机 libc：malloc/free/memcpy/sprintf/str* 等 */
#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/core/zm_mem.h"
#include "./zmaee/core/zm_root.h"
#include "./zmaee/core/zm_str.h"
#include "./zmaee/fs/zm_file_mgr.h"
#include "./zmaee/fs/zm_file.h"
#include "./zmaee/gfx/zm_display.h" /* ZMAEE IDisplay / IBitmap + SDL 渲染后端 */
#include "./zmaee/gfx/zm_image.h"   /* ZMAEE IImage（资源加载链） */
#include "emu_bitmap_traps.h" /* ZMAEE IBitmap 原生虚表槽位（BITMAP_VT_ADDR） */
#include "./zmaee/runtime/shell/zm_shell.h"
#include "./zmaee/runtime/timer/zm_timer.h" /* IShell 定时器子系统 */
#include "event.h"
#include "zmaee/inc/zm_event_code.h"

/*
 * ==========================================================================
 * 客户机堆 / libc 的接线层
 * ==========================================================================
 * applet 通过 ROOT_TABLE_ADDR vtable 调用的这些槽位，语义上就是标准 C 库函数。
 * 这里把它们统一转接到 src/ulibc（跨地址空间的 libc 实现），
 * 由 ulibc 负责"客户机指针搬运"的全部脏活。
 *
 * 默认使用 ulibc 真实堆（g_ulibc_heap=1）：malloc 从空闲链表分配、
 * free 真正回收并与相邻空闲块合并，与原始固件的堆行为一致。
 * ZM_ULIBC_HEAP=0 回退到 bump 分配器，仅用于排查 applet 的
 * UAF / double-free（见 emu.h）。
 * ==========================================================================
 */

/** applet 的 malloc */
static uint32_t applet_malloc(uc_engine *uc, uint32_t size) {
  if (g_ulibc_heap)
    return u_malloc(uc, size);
  return host_malloc(&g_heap_ptr, size); /* 排查用：只增不减 */
}

/** applet 的 free */
static void applet_free(uc_engine *uc, uint32_t p) {
  if (g_ulibc_heap) {
    u_free(uc, p);
    return;
  }
  (void)uc;
  /* 排查模式：不回收，用于确认崩溃是否由内存回收引起 */
}

/* create_cbk 对象里内嵌的“applet 自管堆”初始化。
 *
 * RE（applet 000004051/00000502）：
 *   0x1D6BC() 尾调用 ROOT[0x154]=create_cbk → 拿到 CBK_OBJ
 *   r0 = [CBK_OBJ + 0x4C]            ← 分配器对象
 *   0x15DDC(分配器, size)：
 *       [分配器+0] = 堆管理器；0x175B8(管理器, size) 才真正切内存
 *       小对象走桶：桶头在 分配器+4/+0x10/+0x1C/+0x28，每桶 12 字节（+4 空闲头、+8 节点步长）
 *   0x175B8(管理器, size)：按块链表分配
 *       管理器 { +4 首块地址, +8 区末地址 }
 *       块头 8 字节：+0 u16 魔数 0xCAFE、+2 u8 已用标志、+4 u32 负载大小；负载从 +8 起
 *
 * 这个字段我们以前**从没初始化**（还被 SHIM 填充当成跳板写入过），于是 applet
 * 分配失败 → 拿到 NULL 对象 → 之后 NULL 解引用 → 崩（pc=0x7B080C 那条）。
 * 这里按上面的格式建一个空堆：一整块空闲块，让 0x175B8 自己去切。只做一次。 */
static void cbk_heap_init_once(uc_engine *uc) {
  static int done = 0;
  if (done)
    return;
  done = 1;

  /* 0x15DDC 每次“补桶”会向管理器要 0x8000(32KB)（RE：0x15E1C `mov r1,#128,#28`
   * = 0x8000），128KB 几个桶就见底 → 分配返回 0 → applet 拿到 NULL 后又去
   * Release(0) → 崩（实测 pc=0x173D4 / 0x52069AD8）。给足 1MB。 */
  /* 堆大小可用 ZM_HEAP_KB 调（排查用；默认 0x1F000，给管理器/分配器留头部） */
  const char *ekb = getenv("ZM_HEAP_KB");
  uint32_t HEAP_BYTES =
      (ekb && atoi(ekb) > 0) ? (uint32_t)atoi(ekb) * 1024u : 0x1F000u;
  /* 布局：专用区开头放管理器(0x10) + 分配器(0x40)，其余留给堆区。
   * ★ 既不能向 applet 自己的堆要内存（`applet_malloc`/u_malloc：会把 applet
   * 堆起点整体后移，00000001 立刻崩），也不能放在 blob 里（会与 applet 的
   * 静态数据/我们自己造的堆对象撞车）。用独立的 CBKHEAP 映射区（见
   * emu_mem_regions.h）。 */
  /* 管理器放到**专用区尾部**的保留块里（0x100 字节）：
   * 有些 applet（00000710 实测）会做 `[[CBK_OBJ+0x4C]] → [...+0x30] → blx`，
   * 即把 [CBK+0x4C] 当"带虚表的对象"用；而 00000502 那条路又把 [CBK+0x4C]
   * 的 [+0] 当**堆管理器**读（0x15DDC: `ldr r0,[r0]`）。
   * 两者要同时满足 ⇒ 让 [+0] 仍指管理器，但把管理器搬到一个**独立保留块**，
   * 这样它的 +0x30 就不与分配器的桶字段重叠，可以安全地填一个跳板。 */
  uint32_t mgr = CBKHEAP_BASE + CBKHEAP_SIZE - 0x100u;  /* 保留块（尾部 0x100） */
  uint32_t al = CBKHEAP_BASE + 0x10u;                   /* 分配器 0x40 字节 */
  uint32_t region = CBKHEAP_BASE + 0x50u;               /* 堆区起点 */
  if (HEAP_BYTES > CBKHEAP_SIZE - 0x150u)               /* 给开头 0x50 + 尾部 0x100 */
    HEAP_BYTES = CBKHEAP_SIZE - 0x150u;
  if (HEAP_BYTES < 0x8000u) {
    log_warn("cbk 堆初始化失败：区太小(%u)", HEAP_BYTES);
    return;
  }
  /* 管理器的 +0x30 填一个**有效跳板**：00000710 这类 applet 会
   * `r0=[ctx+0x4C]; r1=1; r2=[[r0]+0x30]; blx r2`（结果按字节当 bool 用）。
   * 先填"返回 0"的中性桩（见 emu_root_traps.h 的 ZM_x3C），保证不再跳飞。 */
  {
    uint32_t stub = TR_root_x3C;
    uc_mem_write(uc, mgr + 0x30, &stub, 4);
  }

  /* 分配器与管理器清零（桶的 [0]=块链、[+4]=空闲头 全 0 = 空） */
  {
    static uint8_t zb[0x100];
    uint32_t n = 0x40 + 0x10;
    for (uint32_t o = 0; o < n; o += sizeof(zb)) {
      uint32_t c = (n - o > sizeof(zb)) ? (uint32_t)sizeof(zb) : (n - o);
      uc_mem_write(uc, al + o, zb, c);
    }
  }

  /* ★ 每个桶的“节点步长”（+8）必须填对：RE 0x15E40 `ldr r2,[r4,#8]`、
   * 0x15E48 `节点总大小 = 步长 + 8`。全 0 时切出来的节点只有 8 字节，
   * 而 applet 要 4/8/0x10/0x20 → 立刻溢出。
   * 桶基址（RE 0x15DF0..0x15E0C）：al+4(≤4) / al+0x10(≤8) / al+0x1C(≤0x10) / al+0x28(≤0x20) */
  {
    static const uint32_t stride[4] = {4u, 8u, 0x10u, 0x20u};
    static const uint32_t boff[4] = {0x04u, 0x10u, 0x1Cu, 0x28u};
    for (int k = 0; k < 4; k++) {
      uint32_t v = stride[k];
      uc_mem_write(uc, al + boff[k] + 8, &v, 4);
    }
  }

  /* 先把整块区清零：applet_malloc 只做 bump、不清零，切成的小块里会残留旧数据，
   * 表现为对象某个成员是垃圾指针（实测 Release 时 [obj+0]=0x52069AD8 越界崩）。 */
  {
    static uint8_t zb[0x400];
    for (uint32_t o = 0; o < HEAP_BYTES; o += sizeof(zb)) {
      uint32_t c = (HEAP_BYTES - o > sizeof(zb)) ? (uint32_t)sizeof(zb)
                                                 : (HEAP_BYTES - o);
      uc_mem_write(uc, region + o, zb, c);
    }
  }

  /* 一整块空闲块：magic 0xCAFE、**标志 1 = 空闲可用**（RE：0x175F0 `bne 0x1768C`
   * 即标志 != 1 就跳过该块 —— 所以 1 才是"可分配"）、负载 = 区大小 - 8 */
  {
    uint8_t hdr[8] = {0};
    uint16_t magic = 0xCAFE;
    uint32_t size = HEAP_BYTES - 8;
    memcpy(hdr, &magic, 2);
    hdr[2] = 1;
    memcpy(hdr + 4, &size, sizeof(size));
    uc_mem_write(uc, region, hdr, sizeof(hdr));
  }

  /* 管理器 { +4 首块, +8 区末 }；分配器 [0] = 管理器 */
  {
    uint32_t first = region;
    /* 【已试并撤回】曾把"区末"取成跳板地址（TRAMP_BASE+0x3C），想让它同时满足
     * "分配上限"与"对象槽 [8] 可调用"两件事 —— 实测 20 个 applet 全部变成
     * 0~1 秒 `exit=1`（不崩但直接走错误分支），属**回退** ✗。保持真值。 */
    uint32_t end = region + HEAP_BYTES;
    uc_mem_write(uc, mgr + 4, &first, 4);
    uc_mem_write(uc, mgr + 8, &end, 4);
    uc_mem_write(uc, al, &mgr, 4);
    /* 【已试并撤回】把 +8 也换成跳板（想让它兼作"对象槽 [8]"）：实测分配器
     * 立刻坏 —— 00000502 退到 0x7B080C、000007xx 全家退回 0x18/0x78 ✗。
     * 说明 +8 必须是真"区末"，与 00000710 想要的"可调用槽 [8]"**硬冲突** ✗。
     * 保留真值；想复现那次实验可设 ZM_STUB8=1。 */
    if (getenv("ZM_STUB8")) {
      uint32_t stub = TR_root_x3C;
      uc_mem_write(uc, mgr + 8, &stub, 4);
    }
  }
  
    if (!getenv("ZM_NO_CBKPTR"))
    uc_mem_write(uc, CBK_OBJ + 0x4C, &al, 4);
  log_info("create_cbk 自管堆已建：分配器=0x%X 管理器=0x%X 区=0x%X..0x%X", al, mgr,
           region, region + HEAP_BYTES);
}

/** applet 的 calloc：分配并清零（清零在客户机侧完成，不开宿主临时缓冲） */
static uint32_t applet_calloc(uc_engine *uc, uint32_t n, uint32_t size) {
  uint32_t p = applet_malloc(uc, n * size);
  if (p)
    u_memset(uc, p, 0, n * size);
  return p;
}

uint32_t getArg(uc_engine *uc, uint32_t n) {
  uint64_t v64 = 0; // 关键：必须用 64 位容器
  uc_err err;

  // 1. 寄存器参数 R0 ~ R3
  if (n <= 3) {
    err = uc_reg_read(uc, UC_ARM_REG_R0 + n, &v64);
    if (err != UC_ERR_OK)
      return 0;
    return (uint32_t)v64;
  }

  // 2. 栈参数（前提：当前 PC 必须在函数第一条指令！）
  uint64_t sp = 0;
  err = uc_reg_read(uc, UC_ARM_REG_SP, &sp);
  if (err != UC_ERR_OK)
    return 0;
  if (sp == STACK_TOP) {
    log_info("sp is STACK_TOP");
    return 0;
  }
  // 第 n 个参数（n>=4）在入口栈顶的偏移 (n-4)*4
  uint64_t addr = sp + (n - 4) * 4;
  uint32_t result = 0;
  err = uc_mem_read(uc, addr, &result, 4);
  if (err != UC_ERR_OK)
    return 0;

  return result;
}
/**
 * 打印 Unicorn 模拟器中当前所有非零的 ARM32 核心寄存器
 * @param uc Unicorn 引擎句柄
 */
void print_non_zero_registers(uc_engine *uc) {
  // 定义需要遍历的寄存器结构
  typedef struct {
    int reg_id;       // Unicorn 定义的寄存器 ID (UC_ARM_REG_*)
    const char *name; // 寄存器名称（用于打印）
  } arm_reg_entry;

  // 针对 MT6250 (ARM7EJ-S) 的核心寄存器列表
  // 去掉了浮点/NEON/Cortex-M专有寄存器，因为MT6250通常不支持或不存在
  arm_reg_entry regs[] = {// 通用寄存器 R0 - R12
                          {UC_ARM_REG_R0, "R0"},
                          {UC_ARM_REG_R1, "R1"},
                          {UC_ARM_REG_R2, "R2"},
                          {UC_ARM_REG_R3, "R3"},
                          {UC_ARM_REG_R4, "R4"},
                          {UC_ARM_REG_R5, "R5"},
                          {UC_ARM_REG_R6, "R6"},
                          {UC_ARM_REG_R7, "R7"},
                          {UC_ARM_REG_R8, "R8"},
                          {UC_ARM_REG_R9, "R9"},
                          {UC_ARM_REG_R10, "R10"},
                          {UC_ARM_REG_R11, "R11"},
                          {UC_ARM_REG_R12, "R12"},
                          // 栈指针、链接寄存器、程序计数器
                          {UC_ARM_REG_SP, "SP"}, // R13
                          {UC_ARM_REG_LR, "LR"}, // R14
                          {UC_ARM_REG_PC, "PC"}, // R15
                                                 // 状态寄存器
                          {UC_ARM_REG_CPSR, "CPSR"},
                          {UC_ARM_REG_SPSR, "SPSR"}};

  int reg_count = sizeof(regs) / sizeof(regs[0]);
  uint32_t value = 0;
  uc_err err = UC_ERR_OK;
  int has_non_zero = 0; // 标记是否有非零寄存器

  printf("========== Non-zero Registers ==========\n");

  for (int i = 0; i < reg_count; i++) {
    // 读取寄存器值
    err = uc_reg_read(uc, regs[i].reg_id, &value);

    if (err == UC_ERR_OK) {
      if (value != 0) {
        printf("[+] %s: 0x%08X (%u)\n", regs[i].name, value, value);
        has_non_zero = 1;
      }
    } else {
      // 读取失败（例如在用户模式下读取 SPSR 会报错），静默跳过即可
      // 如果需要调试，可以取消下面注释：
      // printf("[!] Read %s failed (error: %d)\n", regs[i].name, err);
    }
  }

  if (!has_non_zero) {
    printf("(No non-zero registers found in current context.)\n");
  }
  printf("========================================\n");
}
/* =========================================================================
 * trap 分派表（表驱动）
 *
 * 旧实现是 1100+ 行的巨型 switch：手写分派既长，又容易出复制粘贴缺陷
 * （例如曾把一段无 case 标签的 `ret = zm_shell_stub(...)` 落在
 * TR_shell_GetAppDir 之后、永远不可达——已删除）。改为“陷阱地址 → 处理函数”
 * 的分派表：
 *   - 每个 handler 统一签名 trap_fn(trap_ctx*)；
 *   - 连续的一组 stub 用 [lo, hi] 区间表示，避免逐条列举；
 *   - 控制流类 trap（init / abort / enter_event_loop 等）在 ctx.handled 置位后
 *     自行写 PC / 停 emu，分发器不再写 R0/PC。
 * 新增一个槽位 = 表内加一行，不再需要复制粘贴 case 模板。
 * ========================================================================= */

typedef struct {
  uc_engine *uc;
  uint32_t trap, r0, r1, r2, r3, lr, sp;
  bool handled; /* 控制流类 trap 置位：已自行写 PC / 停 emu，勿再写 R0/PC */
} trap_ctx;

typedef uint32_t (*trap_fn)(trap_ctx *c);

/* 统一适配器宏：实函数签名各异，这里只截取需要的寄存器。 */
#define AD_R0(fn)   static uint32_t a_##fn(trap_ctx *c) { return fn(c->uc, c->r0); }
#define AD_R1(fn)   static uint32_t a_##fn(trap_ctx *c) { return fn(c->uc, c->r1); }
#define AD_R01(fn)  static uint32_t a_##fn(trap_ctx *c) { return fn(c->uc, c->r0, c->r1); }
#define AD_R012(fn) static uint32_t a_##fn(trap_ctx *c) { return fn(c->uc, c->r0, c->r1, c->r2); }
#define AD_R0123(fn) static uint32_t a_##fn(trap_ctx *c) { return fn(c->uc, c->r0, c->r1, c->r2, c->r3); }
#define AD_0(fn)    static uint32_t a_##fn(trap_ctx *c) { return fn(c->uc); }
/* 带字面量 off 的 handler（display / 各类 stub） */
#define AD_OFF(fn, off, tag) static uint32_t a_off_##tag(trap_ctx *c) { return fn(c->uc, (off), c->r0, c->r1, c->r2, c->r3); }

/* ---- 直接转发的 handler（签名各异，由宏统一收口）---- */
AD_R0(zm_shell_AddRef)
AD_R0(zm_shell_Release)
AD_R1(zm_shell_GetDeviceInfo)
AD_0(zm_shell_GetRootDir)
AD_0(zm_shell_GetWorkDir)
AD_R1(zm_shell_CloseApplet)
AD_R1(zm_shell_GetApplet)
AD_R01(zm_strcmp)
AD_R1(zm_timer_CancelTimer)
AD_R1(zm_timer_CancelOwnerTimer)
/* ROOT+0x148/0x14C：applet 级周期定时器（ZMAEE_Start_Timer/Stop_Timer）
 * 签名 RE：Start_Timer(r0=interval_ms, r1=id, r2=cb)；Stop_Timer(r0=id) */
AD_R012(zm_timer_StartTimer)
AD_R0(zm_timer_StopTimer)
AD_0(zm_shell_GetTickCount)
AD_R1(zm_shell_UnloadDLL)
AD_R0123(zm_shell_LoadLibraryExt)
AD_R0(zm_file_close)
AD_R01(zm_fileMgr_GetFreeSize)
AD_R012(zm_file_read)
AD_R012(zm_file_write)
AD_R012(zm_file_seek)
AD_R0(zm_file_tell)
AD_R0(zm_display_AddRef)
AD_R0(zm_display_Release)
AD_R0(zm_display_GetMaxLayerCount)
AD_R0(zm_display_FreeAllLayer)
AD_R0(zm_display_GetActiveLayer)
AD_R0(zm_display_UnlockScreen)
AD_0(zm_display_GetFontHeight)
AD_R01(zm_display_FreeLayer)
AD_R01(zm_display_SetActiveLayer)
AD_R01(zm_display_SelectFont)
AD_R01(zm_display_GetFontWidth)
AD_R01(zm_display_LoadBitmap)
AD_R012(zm_display_SetTransColor)
AD_0(zm_display_Refresh)
AD_R0123(zm_display_UpdateEx)
AD_R0123(zm_display_CreateBitmap)
AD_R0(zm_surf_release)
AD_R01(zm_surf_getrect)
AD_R0(zm_image_AddRef)
AD_R0(zm_image_Release)
AD_R0(zm_image_GetFrameCount)
AD_R0(zm_image_Width)
AD_R0(zm_image_Height)
AD_R0(zm_image_GetType)
AD_R0123(zm_image_SetData)
AD_R0(zm_bitmap_AddRef)
AD_R0(zm_bitmap_Release)
AD_R01(zm_bitmap_SetTransColor)
AD_R01(zm_bitmap_GetInfo)
AD_0(zm_svc_release)
AD_R012(zm_fileMgr_GetInfo)
AD_R01(zm_fileMgr_TestFile)
AD_R01(zm_fileMgr_StorageSupport)
AD_R1(zm_fileMgr_make_dir) /* +0x14 mkdir（安卓 sub_19720 / 手机版 0x111AC 双向确认）*/
AD_R0123(zm_root_x68C)
AD_R0123(zm_ucs2_to_utf8)
AD_R0123(zm_utf8_to_ucs2)
AD_R01(zm_root_str_assign)
AD_R01(zm_strstr)
AD_R01(zm_strchr)
AD_R01(zm_spec_lookup)
AD_R0(zm_strlen)
AD_R0(zm_wcslen)
AD_R012(u_memcmp)
AD_R012(u_memcpy)
AD_R012(u_memset)
AD_R0123(zm_root_cbk_default)
AD_R0123(zm_netmgr_x1C)
AD_R0123(zm_tapi_x2C)
AD_R0123(zm_tapi_x40)
AD_0(zm_dll_init)
AD_0(zm_media_stop)
AD_0(zm_media_pause_music)
AD_0(zm_media_resume_music)

/* 带字面量 off 的 handler（display / 各类 stub） */
AD_OFF(zm_shell_stub, 0x0C, shell_0C)
AD_OFF(zm_shell_stub, 0x18, shell_18)
AD_OFF(zm_shell_stub, 0x20, shell_20)
AD_OFF(zm_shell_stub, 0x28, shell_28)
AD_OFF(zm_shell_stub, 0x2C, shell_2C)
AD_OFF(zm_shell_stub, 0x34, shell_34)
AD_OFF(zm_shell_stub, 0x38, shell_38)
AD_OFF(zm_shell_stub, 0x4C, shell_4C)
AD_OFF(zm_shell_stub, 0x50, shell_50)
AD_OFF(zm_shell_stub, 0x54, shell_54)
AD_OFF(zm_shell_stub, 0x60, shell_60)
AD_OFF(zm_shell_stub, 0x64, shell_64)
AD_OFF(zm_shell_stub, 0x68, shell_68)
AD_OFF(zm_shell_stub, 0x6C, shell_6C)
AD_OFF(zm_shell_stub, 0x70, shell_70)
AD_OFF(zm_shell_stub, 0x74, shell_74)
AD_OFF(zm_shell_stub, 0x7C, shell_7C)
AD_OFF(zm_shell_stub, 0x84, shell_84)
AD_OFF(zm_fileMgr_stub, 0x00, fm_00)
AD_OFF(zm_fileMgr_stub, 0x04, fm_04)
AD_OFF(zm_fileMgr_stub, 0x10, fm_10)
AD_OFF(zm_fileMgr_stub, 0x14, fm_14)
AD_OFF(zm_fileMgr_stub, 0x18, fm_18)
AD_OFF(zm_fileMgr_stub, 0x1C, fm_1C)
AD_OFF(zm_fileMgr_stub, 0x24, fm_24)
AD_OFF(zm_fileMgr_stub, 0x28, fm_28)
AD_OFF(zm_fileMgr_stub, 0x2C, fm_2C)
AD_OFF(zm_fileMgr_stub, 0x34, fm_34)
AD_OFF(zm_fileMgr_stub, 0x38, fm_38)
AD_OFF(zm_display_CreateLayer, 0x0CU, disp_CreateLayer)
AD_OFF(zm_display_CreateLayerExt, 0x10U, disp_CreateLayerExt)
AD_OFF(zm_display_GetLayerInfo, 0x1CU, disp_GetLayerInfo)
AD_OFF(zm_display_SetLayerPosition, 0x24U, disp_SetLayerPosition)
AD_OFF(zm_display_stub, 0x34, disp_LockScreen)
AD_OFF(zm_display_RegisterCustomFont, 0x3CU, disp_RegisterCustomFont)
AD_OFF(zm_display_SetOpacity, 0x58U, disp_SetOpacity)
AD_OFF(zm_display_SetClipRect, 0x5CU, disp_SetClipRect)
AD_OFF(zm_display_GetClipRect, 0x60U, disp_GetClipRect)
AD_OFF(zm_display_SetPixel, 0x64U, disp_SetPixel)
AD_OFF(zm_display_DrawLine, 0x68U, disp_DrawLine)
AD_OFF(zm_display_DrawRoundRect, 0x74U, disp_DrawRoundRect)
AD_OFF(zm_display_DrawCircle, 0x78U, disp_DrawCircle)
AD_OFF(zm_display_FillCircle, 0x7CU, disp_FillCircle)
AD_OFF(zm_display_DrawArc, 0x80U, disp_DrawArc)
AD_OFF(zm_display_FillArc, 0x84U, disp_FillArc)
AD_OFF(zm_display_FillGradientRect, 0x88U, disp_FillGradientRect)
AD_OFF(zm_display_AlphaBlendRect, 0x8CU, disp_AlphaBlendRect)
AD_OFF(zm_display_DrawImage, 0x90U, disp_DrawImage)
AD_OFF(zm_display_DrawBitmap, 0x94U, disp_DrawBitmap)
AD_OFF(zm_display_DrawBitmapFrame, 0x9CU, disp_DrawBitmapFrame)
AD_OFF(zm_display_Flatten, 0xB0U, disp_Flatten)
AD_OFF(zm_display_CreateImage, 0xA8U, disp_CreateImage)
AD_OFF(zm_display_DrawAntialiasingLine, 0xB8U, disp_DrawAntialiasingLine)
AD_OFF(zm_display_DrawWLine, 0xBCU, disp_DrawWLine)
AD_OFF(zm_display_GetDMLayerHdlr, 0xC0U, disp_GetDMLayerHdlr)
AD_OFF(zm_display_RelevanceLayer, 0xC4U, disp_RelevanceLayer)
AD_OFF(zm_display_DrawImageExt, 0xCCU, disp_DrawImageExt)
AD_OFF(zm_display_DrawSysWallPaper, 0xD0U, disp_DrawSysWallPaper)
AD_OFF(zm_display_DrawBorderText, 0xD4U, disp_DrawBorderText)
AD_OFF(zm_display_PushAndSetAlphaLayer, 0xD8U, disp_PushAndSetAlphaLayer)
AD_OFF(zm_display_PopAndRestoreAlphaLayer, 0xDCU, disp_PopAndRestoreAlphaLayer)
AD_OFF(zm_display_RotateScreen, 0xE0U, disp_RotateScreen)
AD_OFF(zm_bitmap_sub_25F78, 0x0CU, bmp_25F78)
AD_OFF(zm_bitmap_sub_25F84, 0x14U, bmp_25F84)
AD_OFF(zm_bitmap_sub_25FF8, 0x18U, bmp_25FF8)
AD_OFF(zm_media_stub, 0x08, media_08)
AD_OFF(zm_media_stub, 0x0C, media_0C)
AD_OFF(zm_media_stub, 0x20, media_20)
AD_OFF(zm_media_stub, 0x24, media_24)
AD_OFF(zm_media_stub, 0x28, media_28)
AD_OFF(zm_media_stub, 0x2C, media_2C)
AD_OFF(zm_media_stub, 0x30, media_30)
AD_OFF(zm_media_stub, 0x34, media_34)
AD_OFF(zm_media_stub, 0x3C, media_3C)
AD_OFF(zm_media_stub, 0x44, media_44)
AD_OFF(zm_media_stub, 0x48, media_48)
AD_OFF(zm_media_stub, 0x4C, media_4C)
AD_OFF(zm_media_stub, 0x50, media_50)
AD_OFF(zm_media_stub, 0x58, media_58)
AD_OFF(zm_setting_stub, 0x08, set_08)
AD_OFF(zm_setting_stub, 0x0C, set_0C)
AD_OFF(zm_setting_stub, 0x10, set_10)
AD_OFF(zm_setting_stub, 0x14, set_14)
AD_OFF(zm_setting_stub, 0x28, set_28)
AD_OFF(zm_setting_stub, 0x2C, set_2C)
AD_OFF(zm_setting_stub, 0x30, set_30)
AD_OFF(zm_setting_stub, 0x34, set_34)

/* ---- 需要特殊处理的 handler（带栈参 / 变参 / 控制流）---- */
static uint32_t a_shell_CreateInstance(trap_ctx *c) {
  return zm_shell_CreateInstance(c->uc, c->r1, c->r2);
}
static uint32_t a_shell_LoadDLL(trap_ctx *c) {
  return zm_shell_LoadDLL(c->uc, c->r1, c->r2, c->r3);
}
static uint32_t a_shell_GetAppDir(trap_ctx *c) {
  return zm_shell_GetAppDir(c->uc, c->r1, c->r2);
}
static uint32_t a_shell_SetTimer(trap_ctx *c) {
  return zm_timer_SetTimer(c->uc, c->r1, c->r2, c->r3, uc_read32(c->uc, c->sp));
}
static uint32_t a_fileMgr_open_file(trap_ctx *c) {
  if (c->r0 == 0)
    return 0;
  return zm_fileMgr_open_file(c->uc, c->r1);
}
static uint32_t a_fileMgr_x3C(trap_ctx *c) {
  log_debug("[FileMgr+0x3C] r0=0x%X r1=0x%X r2=0x%X r3=0x%X lr=0x%X", c->r0, c->r1,
            c->r2, c->r3, c->lr);
  return zm_fileMgr_stub(c->uc, 0x3C, c->r0, c->r1, c->r2, c->r3);
}
static uint32_t a_display_Update(trap_ctx *c) {
  return zm_display_Update(c->uc, c->r0, c->r1, c->r2, c->r3, getArg(c->uc, 4));
}
static uint32_t a_display_MeasureString(trap_ctx *c) {
  return zm_display_MeasureString(c->uc, c->r0, c->r1, c->r2, c->r3, c->sp);
}
static uint32_t a_display_DrawText(trap_ctx *c) {
  return zm_display_DrawText(c->uc, c->r1, c->r2, c->r3, c->sp);
}
static uint32_t a_display_DrawRect(trap_ctx *c) {
  return zm_display_DrawRect(c->uc, c->r1, c->r2, c->r3, c->sp);
}
static uint32_t a_display_FillRect(trap_ctx *c) {
  return zm_display_FillRect(c->uc, c->r1, c->r2, c->r3, c->sp);
}
static uint32_t a_display_DrawBitmapEx(trap_ctx *c) {
  if (g_disasm) {
    static int watch_done = 0;
    if (!watch_done && c->r3 >= 0x820000) {
      uint32_t b0 = uc_read32(c->uc, c->r3), b1 = uc_read32(c->uc, c->r3 + 4);
      uint32_t pc0 = 0;
      uc_reg_read(c->uc, UC_ARM_REG_R4, &pc0);
      log_debug("WATCH bmp=0x%X first=[%08X %08X] R4=0x%X", c->r3, b0, b1, pc0);
      watch_done = 1;
    }
    uint32_t b0 = uc_read32(c->uc, c->r3), b1 = uc_read32(c->uc, c->r3 + 4);
    uint32_t b2 = uc_read32(c->uc, c->r3 + 8);
    log_debug("DrawBitmapEx lr=0x%X x=%u y=%u bmp=0x%X(=[%08X %08X %08X]) "
              "rect=0x%X mode=%u",
              c->lr, c->r1, c->r2, c->r3, b0, b1, b2, getArg(c->uc, 4), getArg(c->uc, 5));
  }
  return zm_display_DrawBitmapEx(c->uc, 0x98U, c->r0, c->r1, c->r2, c->r3);
}
static uint32_t a_display_BitBlt(trap_ctx *c) {
  if (g_disasm) {
    uint32_t s0 = uc_read32(c->uc, c->r3), s1 = uc_read32(c->uc, c->r3 + 4);
    uint32_t s2 = uc_read32(c->uc, c->r3 + 8), s3 = uc_read32(c->uc, c->r3 + 12);
    log_debug("BitBlt lr=0x%X dx=%u dy=%u surf=0x%X(=[%08X %08X %08X %08X]) "
              "rect=0x%X mode=%u flags=%u",
              c->lr, c->r1, c->r2, c->r3, s0, s1, s2, s3, getArg(c->uc, 4), getArg(c->uc, 5),
              getArg(c->uc, 6));
  }
  return zm_display_BitBlt(c->uc, 0xACU, c->r0, c->r1, c->r2, c->r3);
}
static uint32_t a_display_StretchBlt(trap_ctx *c) {
  static uint32_t sn = 0;
  if (sn++ < 6)
    log_info("[StretchBlt] lr=0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X", c->lr, c->r0, c->r1,
             c->r2, c->r3);
  return zm_display_StretchBlt(c->uc, 0xB4U, c->r0, c->r1, c->r2, c->r3);
}
static uint32_t a_image_Decode(trap_ctx *c) {
  return zm_image_DecodeToBitmap(c->uc, c->r0, c->r1, c->r2, c->r3, c->sp);
}
static uint32_t a_media_play(trap_ctx *c) {
  return zm_media_command(c->uc, c->r0, c->r1, c->r2, c->r3, c->sp);
}
static uint32_t a_media_x54(trap_ctx *c) {
  return zm_media_command(c->uc, c->r0, c->r1, c->r2, c->r3, c->sp);
}
static uint32_t a_media_AddRef(trap_ctx *c) {
  log_info("IMedia.AddRef called");
  return 1;
}
static uint32_t a_media_Release(trap_ctx *c) {
  log_info("IMedia.Release called");
  return zm_svc_release(c->uc);
}
static uint32_t a_media_x38(trap_ctx *c) {
  (void)c;
  return (uint32_t)-1;
}
static uint32_t a_media_x40(trap_ctx *c) {
  (void)c;
  return (uint32_t)-1;
}
static uint32_t a_setting_AddRef(trap_ctx *c) {
  (void)c;
  return 1;
}
static uint32_t a_setting_x18(trap_ctx *c) {
  log_info("ISetting[0x18] lr=0x%X on=%u", c->lr, c->r1);
  return zm_setting_set_sound(c->uc, c->r1);
}
static uint32_t a_setting_x1C(trap_ctx *c) {
  if (g_disasm)
    log_debug("ISetting[0x1C] 调用点 lr=0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X",
              c->lr, c->r0, c->r1, c->r2, c->r3);
  return zm_setting_stub(c->uc, 0x1C, c->r0, c->r1, c->r2, c->r3);
}
static uint32_t a_setting_x20(trap_ctx *c) {
  if (g_disasm)
    log_debug("ISetting[0x20] 调用点 lr=0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X",
              c->lr, c->r0, c->r1, c->r2, c->r3);
  return zm_setting_stub(c->uc, 0x20, c->r0, c->r1, c->r2, c->r3);
}
static uint32_t a_setting_x24(trap_ctx *c) {
  return zm_setting_x24(c->uc, c->r1, c->r2);
}
static uint32_t a_dll_config(trap_ctx *c) {
  return zm_dll_config(c->uc, c->r1, c->r2, c->r3);
}
static uint32_t a_dll_entry(trap_ctx *c) {
  return zm_dll_entry(c->uc, c->r1, c->r2, c->r3);
}
static uint32_t a_root_getShell(trap_ctx *c) {
  (void)c;
  return G_SHELL_ADDR;
}
static uint32_t a_root_malloc(trap_ctx *c) {
  uint32_t a_size = c->r0;
  uint32_t a_ctx = c->r1;
  uint32_t ret = applet_malloc(c->uc, c->r0);
  log_debug("malloc(size=%u ctx=0x%X lr=0x%X -> 0x%X", a_size, a_ctx, c->lr, ret);
  return ret;
}
static uint32_t a_root_free(trap_ctx *c) {
  log_debug("free(0x%X) lr=0x%X", c->r0, c->lr);
  applet_free(c->uc, c->r0);
  return 0;
}
static uint32_t a_root_malloc_screen(trap_ctx *c) {
  uint32_t ret = applet_malloc(c->uc, c->r0);
  log_debug("MallocScreenMem(0x%X) = 0x%X (lr=0x%X)", c->r0, ret, c->lr);
  return ret;
}
static uint32_t a_root_free_screen(trap_ctx *c) {
  log_debug("FreeScreenMem(0x%X) lr=0x%X", c->r0, c->lr);
  applet_free(c->uc, c->r0);
  return 0;
}
static uint32_t a_root_x3C(trap_ctx *c) {
  (void)c;
  return 0;
}
static uint32_t a_root_sprintf(trap_ctx *c) {
  u_va va;
  u_va_start_mem8(&va, c->uc, c->r2, 0);
  uint32_t ret = (uint32_t)u_sprintf(c->uc, c->r0, c->r1, &va);
  if (g_disasm) {
    char fmt[128], outp[256];
    uint32_t lr = 0;
    uc_reg_read(c->uc, UC_ARM_REG_LR, &lr);
    read_cstr(c->uc, c->r1, fmt, sizeof(fmt));
    read_cstr(c->uc, c->r0, outp, sizeof(outp));
    log_debug("sprintf lr=0x%X [%u参数]: fmt=\"%s\" out=\"%s\" ret=%u", lr,
              uc_read32(c->uc, c->r2), fmt, outp, ret);
  }
  return ret;
}
static uint32_t a_root_str_to_num(trap_ctx *c) {
  return (uint32_t)u_strtol(c->uc, c->r0, c->r1, (int)c->r2);
}

/* 把 double 结果按 ARM EABI 拆成 r0(低)/r1(高)。trap 框架只回写 r0，
 * 高 32 位需 handler 自己写 r1。 */
static uint32_t trap_ret_double(uc_engine *uc, double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  uint32_t hi = (uint32_t)(bits >> 32);
  uc_reg_write(uc, UC_ARM_REG_R1, &hi);
  return (uint32_t)(bits & 0xFFFFFFFFu);
}

/* ROOT_TABLE[0x70] = 字符串→double（CString::ToDouble / atof）。
 * r0 指向以 0 结尾的 ASCII（CString 内联数据在 +0）。 */
static uint32_t a_root_atof(trap_ctx *c) {
  double v = u_strtod_ex(c->uc, c->r0, NULL);
  if (getenv("ZM_SHOW_TRACE"))
    log_info("[atof] str@0x%X => %g", c->r0, v);
  return trap_ret_double(c->uc, v);
}

/* ROOT_TABLE[0x12C] = zmaee 的双精度二元运算（applet 侧包装是 sub_44E8）
 *
 * 调用约定是**标准 AAPCS**：
 *   r0:r1 = 左操作数 lhs（小端：r0 低 32 位、r1 高 32 位）
 *   r2:r3 = 右操作数 rhs
 *   [sp+0] = 运算符选择器：0=+ 1=- 2=× 3=÷（其余返回 0）
 *   返回 r0:r1 = 结果；**由调用方自己 STM 写回内存**，本 handler 不动内存。
 *
 * 【踩过的坑】最早写成"从栈上 sp+32 取操作数地址、再减 8 取另一个"，
 * 那个偏移只对 sub_10B4（四则运算）那个调用点碰巧成立；M+/M- 是另一处
 * 调用点（sub_1D9C 的 loc_25D8），sp+32 根本不是操作数地址，于是取到
 * lhs=rhs=0 —— 表现为"M+ 存不进去、MR 读出来永远是 0（像 MC）"。
 * 按 AAPCS 从寄存器取参后，两个调用点同时正确。
 *
 * 【顺序】必须是 lhs op rhs（先按的 op 后按的）：1-2 得 -1、1÷2 得 0.5。 */
static uint32_t a_root_f_op(trap_ctx *c) {
  int op = (int)uc_read32(c->uc, c->sp + 0);
  uint64_t lb = ((uint64_t)c->r1 << 32) | (uint64_t)c->r0;
  uint64_t rb = ((uint64_t)c->r3 << 32) | (uint64_t)c->r2;
  double lhs = 0.0, rhs = 0.0, r = 0.0;
  memcpy(&lhs, &lb, sizeof(lhs));
  memcpy(&rhs, &rb, sizeof(rhs));
  switch (op) {
  case 0: r = lhs + rhs; break;
  case 1: r = lhs - rhs; break;
  case 2: r = lhs * rhs; break;
  case 3: r = (rhs != 0.0) ? lhs / rhs : 0.0; break;
  default: r = 0.0; break;
  }
  if (getenv("ZM_SHOW_TRACE"))
    log_info("[f_op] op=%d %g <op> %g => %g (lr=0x%X)", op, lhs, rhs, r, c->lr);
  return trap_ret_double(c->uc, r);
}
static uint32_t a_root_str_ctor(trap_ctx *c) {
  return u_strcpy(c->uc, c->r0, c->r1);
}
static uint32_t a_root_srand(trap_ctx *c) {
  zm_root_srand(c->r0);
  return 0;
}
static uint32_t a_root_rand(trap_ctx *c) {
  (void)c;
  return zm_root_rand();
}
static uint32_t a_root_sqrt(trap_ctx *c) { return zm_root_math(c->uc, ZM_MATH_SQRT, c->r0, c->r1); }
static uint32_t a_root_cos(trap_ctx *c)  { return zm_root_math(c->uc, ZM_MATH_COS, c->r0, c->r1); }
static uint32_t a_root_sin(trap_ctx *c)  { return zm_root_math(c->uc, ZM_MATH_SIN, c->r0, c->r1); }
static uint32_t a_root_atan(trap_ctx *c) { return zm_root_math(c->uc, ZM_MATH_ATAN, c->r0, c->r1); }
static uint32_t a_root_tan(trap_ctx *c)  { return zm_root_math(c->uc, ZM_MATH_TAN, c->r0, c->r1); }
static uint32_t a_root_create_cbk(trap_ctx *c) {
  uint32_t ret = zm_root_create_cbk(c->uc);
  cbk_heap_init_once(c->uc);
  return ret;
}

/* ---- 区间 stub（offset 由 trap 地址自动算）---- */
static uint32_t d_util_stub(trap_ctx *c) {
  return zm_util_stub(c->uc, c->trap - TRAMP_BASE, c->r0, c->r1, c->r2, c->r3);
}
static uint32_t d_surf_nop(trap_ctx *c) {
  return zm_surf_nop(c->uc, c->trap - TRAMP_BASE);
}
static uint32_t d_image_stub(trap_ctx *c) {
  return zm_image_stub(c->uc, c->trap - TRAMP_BASE, c->r0, c->r1, c->r2, c->r3);
}
static uint32_t d_tapi_stub(trap_ctx *c) {
  return zm_tapi_stub(c->uc, c->trap - TAPI_VT_ADDR, c->r0, c->r1, c->r2, c->r3);
}
static uint32_t d_zip_stub(trap_ctx *c) {
  return zm_zip_stub(c->uc, c->trap - ZIP_VT_ADDR, c->r0, c->r1, c->r2, c->r3);
}

/* ---- 控制流类（自行写 PC / 停 emu，置 handled）---- */
static uint32_t a_init_callback(trap_ctx *c) {
  uint32_t a0 = SIZE_SLOT;
  uint32_t a1 = API_SLOT;
  uc_reg_write(c->uc, UC_ARM_REG_R0, &a0);
  uc_reg_write(c->uc, UC_ARM_REG_R1, &a1);
  uint32_t callback_addr = g_registered_loop ? g_registered_loop : TR_enter_event_loop;
  uc_reg_write(c->uc, UC_ARM_REG_LR, &callback_addr);
  uint32_t entry = APPLET_ENTRY_POINT;
  uc_reg_write(c->uc, UC_ARM_REG_PC, &entry);
  log_info("init 握手：r0=&SIZE_SLOT、r1=&API_SLOT，返回地址=0x%X", callback_addr);
  c->handled = true;
  return 0;
}
static uint32_t a_register_event_loop(trap_ctx *c) {
  if (c->r0)
    g_registered_loop = c->r0;
  log_info("applet 注册事件循环入口: 0x%X", c->r0);
  return 0;
}
static uint32_t a_abort(trap_ctx *c) {
  if (g_disasm) {
    char msg[64];
    read_cstr(c->uc, c->r0, msg, sizeof(msg));
    log_info("applet 调用 abort(\"%s\")，停止模拟", msg);
  } else {
    log_info("applet 调用 abort，停止模拟");
  }
  uc_emu_stop(c->uc);
  c->handled = true;
  return 0;
}
/* 定时器异步中断的返回跳板：被打断的现场由 zm_timer.c 保存，
 * 回调执行完 `bx lr` 落到这里 → 恢复现场并回到被打断的那条指令。
 * 必须置 handled：PC 已由恢复逻辑写好，不能让分发器再写 R0/PC。 */
static uint32_t a_timer_return(trap_ctx *c) {
  zm_timer_interrupt_return(c->uc);
  c->handled = true;
  return 0;
}

static uint32_t a_enter_event_loop(trap_ctx *c) {
  static int stage = 0;
  static int resume_enabled = 1;
  static int resume_inited = 0;
  uc_engine *uc = c->uc;
  c->handled = true; /* 该 trap 不走默认 R0/PC 写回 */

  if (zm_event_close_requested()) {
    log_info("applet 的退出回调已跑完（CloseApplet）→ 结束模拟");
    uc_emu_stop(uc);
    return 0;
  }
  if (!resume_inited) {
    const char *ar = getenv("ZM_AUTO_RESUME");
    resume_enabled = (!ar || ar[0] != '0');
    resume_inited = 1;
  }

  if (stage == 0) {
    uint32_t size = uc_read32(uc, SIZE_SLOT);
    uint32_t handler = uc_read32(uc, API_SLOT + 8);
    log_info("init 握手完成：size=%u handler=0x%X（API_SLOT=[%08X %08X %08X "
             "%08X]）",
             size, handler, uc_read32(uc, API_SLOT), uc_read32(uc, API_SLOT + 4),
             uc_read32(uc, API_SLOT + 8), uc_read32(uc, API_SLOT + 12));
    /* 宽松握手补丁：部分 applet（如 Hello World demo）往 SIZE_SLOT 写的
     * 是指针/自引用地址（如 0x7A8100，恰为该槽自身地址）而非整数实例大小，
     * 导致 size 为 0 或远超 0x40000 上限。只要 handler 有效就继续，size 改
     * 用兜底值——实例不可能太大，0x2000 足够且安全。 */
    if (handler == 0) {
      log_error("握手结果异常：handler=0，无法继续");
      uc_emu_stop(uc);
      return 0;
    }
    if (size == 0 || size > 0x40000) {
      const uint32_t FALLBACK_SIZE = 0x2000;
      log_warn("握手 size 异常（0x%X），按宽松握手使用兜底大小 %u 字节继续",
               size, FALLBACK_SIZE);
      size = FALLBACK_SIZE;
    }

    uint32_t blk = applet_calloc(uc, 1, size + 32);
    uint32_t INSTANCE = blk + 16;
    uc_write32(uc, blk + 0, uc_read32(uc, API_SLOT + 0));
    uc_write32(uc, blk + 4, uc_read32(uc, API_SLOT + 4));
    uc_write32(uc, blk + 8, uc_read32(uc, API_SLOT + 8));
    uc_write32(uc, INSTANCE, blk); /* +16 = self */
    const char *filename = get_filename_from_fullpath(g_app_pathname);
    size_t fn_len = strlen(filename) + 1;
    if (4 + fn_len <= (size_t)size + 32u)
      uc_mem_write(uc, INSTANCE + 4, filename, fn_len);
    else
      log_warn("filename (len=%zu) 超出实例大小 %u，跳过写入", fn_len, size);

    g_instance = INSTANCE;
    g_handler = handler;
    log_info("实例已分配：instance=0x%X（%u 字节），handler=0x%X", INSTANCE, size, handler);

    stage = 1;
    log_info("派发 EV_CREATE(evt=0) -> handler=0x%X", g_handler);
    dispatch_applet_event(0 /* EV_CREATE */, 0, 0);
    log_info("EV_CREATE 之后：访问 [CBK_OBJ+0x48] = 0x%X（我们初始化的是 0x%X）",
             uc_read32(uc, CBK_OBJ + 0x48), CBK_CTX);
    return 0;
  }

  if (stage == 1) {
    stage = 2;
    if (resume_enabled) {
      log_info("派发 EV_RESUME(evt=3) -> handler=0x%X", g_handler);
      dispatch_applet_event(3, 0, 0);
      return 0;
    }
  }

  uint32_t hold_ms = 0;
  const char *env = getenv("ZM_GFX_HOLD_MS");
  if (env && *env)
    hold_ms = (uint32_t)strtoul(env, NULL, 0);
  static int exit_dispatched = 0;
  if (exit_dispatched) {
    uc_emu_stop(uc);
    return 0;
  }
  if (!zm_display_event_loop(on_touch_click, hold_ms)) {
    const char *ex = getenv("ZM_EXIT_EVENT");
    if (ex && *ex && ex[0] != '0') {
      exit_dispatched = 1;
      log_info("派发 EV_STOP(evt=1) -> handler=0x%X（让 applet 自己收尾）", g_handler);
      dispatch_applet_event(1, 0, 0);
      return 0;
    }
    uc_emu_stop(uc);
  }
  return 0;
}

/* =========================================================================
 * 分派表：[lo, hi] 区间内的 trap 交给同一个 fn；精确槽位单独成行。
 * 注意：tapi / surf / image 的精确项必须排在各自区间之前，保证优先匹配。
 * ========================================================================= */
static const struct { uint32_t lo, hi; trap_fn fn; } k_trap_table[] = {
  /* 控制流 / 特殊 */
  { TR_timer_return, TR_timer_return, a_timer_return },
  { TR_init_callback, TR_init_callback, a_init_callback },
  { TR_register_event_loop, TR_register_event_loop, a_register_event_loop },
  { TR_abort, TR_abort, a_abort },
  { TR_enter_event_loop, TR_enter_event_loop, a_enter_event_loop },
  { TR_root_start_timer, TR_root_start_timer, a_zm_timer_StartTimer },
  { TR_root_stop_timer, TR_root_stop_timer, a_zm_timer_StopTimer },
  { TR_root_create_cbk, TR_root_create_cbk, a_root_create_cbk },
  { TR_shell_CreateInstance, TR_shell_CreateInstance, a_shell_CreateInstance },
  /* root */
  { TR_root_getShell, TR_root_getShell, a_root_getShell },
  { TR_root_malloc, TR_root_malloc, a_root_malloc },
  { TR_root_free, TR_root_free, a_root_free },
  { TR_root_malloc_screen, TR_root_malloc_screen, a_root_malloc_screen },
  { TR_root_free_screen, TR_root_free_screen, a_root_free_screen },
  { TR_root_x3C, TR_root_x3C, a_root_x3C },
  { TR_root_ucs2_to_utf8, TR_root_ucs2_to_utf8, a_zm_ucs2_to_utf8 },
  { TR_root_utf8_to_ucs2, TR_root_utf8_to_ucs2, a_zm_utf8_to_ucs2 },
  { TR_root_sprintf, TR_root_sprintf, a_root_sprintf },
  { TR_root_str_to_num, TR_root_str_to_num, a_root_str_to_num },
  { TR_root_atof, TR_root_atof, a_root_atof },
  { TR_root_x12C, TR_root_x12C, a_root_f_op },
  { TR_root_str_ctor, TR_root_str_ctor, a_root_str_ctor },
  { TR_root_strcmp, TR_root_strcmp, a_zm_strcmp },
  { TR_root_strchr, TR_root_strchr, a_zm_strlen },
  { TR_root_memcmp, TR_root_memcmp, a_u_memcmp },
  { TR_root_memcpy, TR_root_memcpy, a_u_memcpy },
  { TR_root_memset, TR_root_memset, a_u_memset },
  { TR_root_str_assign, TR_root_str_assign, a_zm_root_str_assign },
  { TR_root_strstr, TR_root_strstr, a_zm_strstr },
  { TR_root_x68C, TR_root_x68C, a_zm_root_x68C },
  { TR_root_spec_lookup, TR_root_spec_lookup, a_zm_spec_lookup },
  { TR_root_str_find, TR_root_str_find, a_zm_strchr },
  { TR_root_wcslen, TR_root_wcslen, a_zm_wcslen },
  { TR_root_srand, TR_root_srand, a_root_srand },
  { TR_root_rand, TR_root_rand, a_root_rand },
  { TR_root_sqrt, TR_root_sqrt, a_root_sqrt },
  { TR_root_cos, TR_root_cos, a_root_cos },
  { TR_root_sin, TR_root_sin, a_root_sin },
  { TR_root_atan, TR_root_atan, a_root_atan },
  { TR_root_tan, TR_root_tan, a_root_tan },
  /* shell */
  { TR_shell_AddRef, TR_shell_AddRef, a_zm_shell_AddRef },
  { TR_shell_Release, TR_shell_Release, a_zm_shell_Release },
  { TR_shell_x0C, TR_shell_x0C, a_off_shell_0C },
  { TR_shell_GetDeviceInfo, TR_shell_GetDeviceInfo, a_zm_shell_GetDeviceInfo },
  { TR_shell_GetRootDir, TR_shell_GetRootDir, a_zm_shell_GetRootDir },
  { TR_shell_SetWorkDir, TR_shell_SetWorkDir, a_off_shell_18 },
  { TR_shell_GetWorkDir, TR_shell_GetWorkDir, a_zm_shell_GetWorkDir },
  { TR_shell_StartApplet, TR_shell_StartApplet, a_off_shell_20 },
  { TR_shell_CloseApplet, TR_shell_CloseApplet, a_zm_shell_CloseApplet },
  { TR_shell_CanStartApplet, TR_shell_CanStartApplet, a_off_shell_28 },
  { TR_shell_ActiveApplet, TR_shell_ActiveApplet, a_off_shell_2C },
  { TR_shell_GetApplet, TR_shell_GetApplet, a_zm_shell_GetApplet },
  { TR_shell_x34, TR_shell_x34, a_off_shell_34 },
  { TR_shell_x38, TR_shell_x38, a_off_shell_38 },
  { TR_shell_SetTimer, TR_shell_SetTimer, a_shell_SetTimer },
  { TR_shell_CancelTimer, TR_shell_CancelTimer, a_zm_timer_CancelTimer },
  { TR_shell_CancelOwnerTimer, TR_shell_CancelOwnerTimer, a_zm_timer_CancelOwnerTimer },
  { TR_shell_GetTickCount, TR_shell_GetTickCount, a_zm_shell_GetTickCount },
  { TR_shell_OpenWapBrowser, TR_shell_OpenWapBrowser, a_off_shell_4C },
  { TR_shell_x50, TR_shell_x50, a_off_shell_50 },
  { TR_shell_SetEndKeyMask, TR_shell_SetEndKeyMask, a_off_shell_54 },
  { TR_shell_LoadDLL, TR_shell_LoadDLL, a_shell_LoadDLL },
  { TR_shell_UnloadDLL, TR_shell_UnloadDLL, a_zm_shell_UnloadDLL },
  { TR_shell_GetAppletMask, TR_shell_GetAppletMask, a_off_shell_60 },
  { TR_shell_SetAppletMask, TR_shell_SetAppletMask, a_off_shell_64 },
  { TR_shell_IsLoadGlobalLibrary, TR_shell_IsLoadGlobalLibrary, a_off_shell_68 },
  { TR_shell_LoadGlobalLibrary, TR_shell_LoadGlobalLibrary, a_off_shell_6C },
  { TR_shell_FreeGlobalLibrary, TR_shell_FreeGlobalLibrary, a_off_shell_70 },
  { TR_shell_IsGlobalLibraryUseStaticMem, TR_shell_IsGlobalLibraryUseStaticMem, a_off_shell_74 },
  { TR_shell_LoadLibraryExt, TR_shell_LoadLibraryExt, a_zm_shell_LoadLibraryExt },
  { TR_shell_EntryApplet, TR_shell_EntryApplet, a_off_shell_7C },
  { TR_shell_GetAppDir, TR_shell_GetAppDir, a_shell_GetAppDir },
  { TR_shell_GetSupportHall, TR_shell_GetSupportHall, a_off_shell_84 },
  /* fileMgr */
  { TR_fileMgr_AddRef, TR_fileMgr_AddRef, a_off_fm_00 },
  { TR_fileMgr_Release, TR_fileMgr_Release, a_off_fm_04 },
  { TR_fileMgr_open_file, TR_fileMgr_open_file, a_fileMgr_open_file },
  { TR_fileMgr_x0C, TR_fileMgr_x0C, a_zm_fileMgr_GetInfo },
  { TR_fileMgr_x10, TR_fileMgr_x10, a_off_fm_10 },
  /* +0x14 = mkdir（原为空桩）。applet 读写数据文件前会逐级建目录
   * （安卓 sub_19720 / 手机版 0x111AC 双向确认），配合 OpenFile 的"新建空文件"
   * 才能让 0000042f 的存档（OpenFile→Write(8B)→Close）真正落盘。 */
  { TR_fileMgr_x14, TR_fileMgr_x14, a_zm_fileMgr_make_dir },
  { TR_fileMgr_x18, TR_fileMgr_x18, a_off_fm_18 },
  { TR_fileMgr_x1C, TR_fileMgr_x1C, a_off_fm_1C },
  { TR_fileMgr_x20, TR_fileMgr_x20, a_zm_fileMgr_TestFile },
  { TR_fileMgr_x24, TR_fileMgr_x24, a_off_fm_24 },
  { TR_fileMgr_x28, TR_fileMgr_x28, a_off_fm_28 },
  { TR_fileMgr_x2C, TR_fileMgr_x2C, a_off_fm_2C },
  { TR_fileMgr_x30, TR_fileMgr_x30, a_zm_fileMgr_StorageSupport },
  /* +0x34 GetFreeSize（RE sub_29E08）：00000442 启动时靠它判断"空间够不够"；
   * 之前是空桩恒返 0 → 永远判成"磁盘空间不足" */
  { TR_fileMgr_x34, TR_fileMgr_x34, a_zm_fileMgr_GetFreeSize },
  { TR_fileMgr_x38, TR_fileMgr_x38, a_fileMgr_x3C },
  { TR_file_close, TR_file_close, a_zm_file_close },
  { TR_file_read, TR_file_read, a_zm_file_read },
  { TR_file_write, TR_file_write, a_zm_file_write },
  { TR_file_seek, TR_file_seek, a_zm_file_seek },
  { TR_file_tell, TR_file_tell, a_zm_file_tell },
  /* util 区间 */
  { TR_util_x00, TR_util_x18, d_util_stub },
  /* display */
  { TR_display_AddRef, TR_display_AddRef, a_zm_display_AddRef },
  { TR_display_Release, TR_display_Release, a_zm_display_Release },
  { TR_display_GetMaxLayerCount, TR_display_GetMaxLayerCount, a_zm_display_GetMaxLayerCount },
  { TR_display_CreateLayer, TR_display_CreateLayer, a_off_disp_CreateLayer },
  { TR_display_CreateLayerExt, TR_display_CreateLayerExt, a_off_disp_CreateLayerExt },
  { TR_display_FreeLayer, TR_display_FreeLayer, a_zm_display_FreeLayer },
  { TR_display_FreeAllLayer, TR_display_FreeAllLayer, a_zm_display_FreeAllLayer },
  { TR_display_GetLayerInfo, TR_display_GetLayerInfo, a_off_disp_GetLayerInfo },
  { TR_display_SetActiveLayer, TR_display_SetActiveLayer, a_zm_display_SetActiveLayer },
  { TR_display_SetLayerPosition, TR_display_SetLayerPosition, a_off_disp_SetLayerPosition },
  { TR_display_Update, TR_display_Update, a_display_Update },
  { TR_display_UpdateEx, TR_display_UpdateEx, a_zm_display_UpdateEx },
  { TR_display_GetActiveLayer, TR_display_GetActiveLayer, a_zm_display_GetActiveLayer },
  { TR_display_LockScreen, TR_display_LockScreen, a_off_disp_LockScreen },
  { TR_display_UnlockScreen, TR_display_UnlockScreen, a_zm_display_UnlockScreen },
  { TR_display_RegisterCustomFont, TR_display_RegisterCustomFont, a_off_disp_RegisterCustomFont },
  { TR_display_SelectFont, TR_display_SelectFont, a_zm_display_SelectFont },
  { TR_display_GetFontWidth, TR_display_GetFontWidth, a_zm_display_GetFontWidth },
  { TR_display_GetFontHeight, TR_display_GetFontHeight, a_zm_display_GetFontHeight },
  { TR_display_MeasureString, TR_display_MeasureString, a_display_MeasureString },
  { TR_display_DrawText, TR_display_DrawText, a_display_DrawText },
  { TR_display_SetTransColor, TR_display_SetTransColor, a_zm_display_SetTransColor },
  { TR_display_SetOpacity, TR_display_SetOpacity, a_off_disp_SetOpacity },
  { TR_display_SetClipRect, TR_display_SetClipRect, a_off_disp_SetClipRect },
  { TR_display_GetClipRect, TR_display_GetClipRect, a_off_disp_GetClipRect },
  { TR_display_SetPixel, TR_display_SetPixel, a_off_disp_SetPixel },
  { TR_display_DrawLine, TR_display_DrawLine, a_off_disp_DrawLine },
  { TR_display_DrawRect, TR_display_DrawRect, a_display_DrawRect },
  { TR_display_FillRect, TR_display_FillRect, a_display_FillRect },
  { TR_display_DrawRoundRect, TR_display_DrawRoundRect, a_off_disp_DrawRoundRect },
  { TR_display_DrawCircle, TR_display_DrawCircle, a_off_disp_DrawCircle },
  { TR_display_FillCircle, TR_display_FillCircle, a_off_disp_FillCircle },
  { TR_display_DrawArc, TR_display_DrawArc, a_off_disp_DrawArc },
  { TR_display_FillArc, TR_display_FillArc, a_off_disp_FillArc },
  { TR_display_FillGradientRect, TR_display_FillGradientRect, a_off_disp_FillGradientRect },
  { TR_display_AlphaBlendRect, TR_display_AlphaBlendRect, a_off_disp_AlphaBlendRect },
  { TR_display_DrawImage, TR_display_DrawImage, a_off_disp_DrawImage },
  { TR_display_DrawBitmap, TR_display_DrawBitmap, a_off_disp_DrawBitmap },
  { TR_display_DrawBitmapEx, TR_display_DrawBitmapEx, a_display_DrawBitmapEx },
  { TR_display_DrawBitmapFrame, TR_display_DrawBitmapFrame, a_off_disp_DrawBitmapFrame },
  { TR_display_CreateBitmap, TR_display_CreateBitmap, a_zm_display_CreateBitmap },
  { TR_display_LoadBitmap, TR_display_LoadBitmap, a_zm_display_LoadBitmap },
  { TR_display_CreateImage, TR_display_CreateImage, a_off_disp_CreateImage },
  { TR_display_BitBlt, TR_display_BitBlt, a_display_BitBlt },
  { TR_display_Flatten, TR_display_Flatten, a_off_disp_Flatten },
  { TR_display_StretchBlt, TR_display_StretchBlt, a_display_StretchBlt },
  { TR_display_DrawAntialiasingLine, TR_display_DrawAntialiasingLine, a_off_disp_DrawAntialiasingLine },
  { TR_display_DrawWLine, TR_display_DrawWLine, a_off_disp_DrawWLine },
  { TR_display_GetDMLayerHdlr, TR_display_GetDMLayerHdlr, a_off_disp_GetDMLayerHdlr },
  { TR_display_RelevanceLayer, TR_display_RelevanceLayer, a_off_disp_RelevanceLayer },
  { TR_display_Refresh, TR_display_Refresh, a_zm_display_Refresh },
  { TR_display_DrawImageExt, TR_display_DrawImageExt, a_off_disp_DrawImageExt },
  { TR_display_DrawSysWallPaper, TR_display_DrawSysWallPaper, a_off_disp_DrawSysWallPaper },
  { TR_display_DrawBorderText, TR_display_DrawBorderText, a_off_disp_DrawBorderText },
  { TR_display_PushAndSetAlphaLayer, TR_display_PushAndSetAlphaLayer, a_off_disp_PushAndSetAlphaLayer },
  { TR_display_PopAndRestoreAlphaLayer, TR_display_PopAndRestoreAlphaLayer, a_off_disp_PopAndRestoreAlphaLayer },
  { TR_display_RotateScreen, TR_display_RotateScreen, a_off_disp_RotateScreen },
  /* surf（精确项须先于区间） */
  { TR_surf_release, TR_surf_release, a_zm_surf_release },
  { TR_surf_getrect, TR_surf_getrect, a_zm_surf_getrect },
  { TR_surf_x04, TR_surf_x50, d_surf_nop },
  /* image */
  { TR_image_AddRef, TR_image_AddRef, a_zm_image_AddRef },
  { TR_image_Release, TR_image_Release, a_zm_image_Release },
  { TR_image_SetData, TR_image_SetData, a_zm_image_SetData },
  { TR_image_GetFrameCount, TR_image_GetFrameCount, a_zm_image_GetFrameCount },
  { TR_image_Width, TR_image_Width, a_zm_image_Width },
  { TR_image_Height, TR_image_Height, a_zm_image_Height },
  { TR_image_GetType, TR_image_GetType, a_zm_image_GetType },
  { TR_image_Decode, TR_image_Decode, a_image_Decode },
  { TR_image_x20, TR_image_x3C, d_image_stub },
  /* bitmap */
  { TR_bitmap_AddRef, TR_bitmap_AddRef, a_zm_bitmap_AddRef },
  { TR_bitmap_Release, TR_bitmap_Release, a_zm_bitmap_Release },
  { TR_bitmap_SetTransColor, TR_bitmap_SetTransColor, a_zm_bitmap_SetTransColor },
  { TR_bitmap_sub_25F78, TR_bitmap_sub_25F78, a_off_bmp_25F78 },
  { TR_bitmap_GetInfo, TR_bitmap_GetInfo, a_zm_bitmap_GetInfo },
  { TR_bitmap_sub_25F84, TR_bitmap_sub_25F84, a_off_bmp_25F84 },
  { TR_bitmap_sub_25FF8, TR_bitmap_sub_25FF8, a_off_bmp_25FF8 },
  /* media */
  { TR_media_AddRef, TR_media_AddRef, a_media_AddRef },
  { TR_media_Release, TR_media_Release, a_media_Release },
  { TR_media_x08, TR_media_x08, a_off_media_08 },
  { TR_media_x0C, TR_media_x0C, a_off_media_0C },
  { TR_media_play, TR_media_play, a_media_play },
  { TR_media_stop, TR_media_stop, a_zm_media_stop },
  { TR_media_x18, TR_media_x18, a_zm_media_pause_music },
  { TR_media_x1C, TR_media_x1C, a_zm_media_resume_music },
  { TR_media_x20, TR_media_x20, a_off_media_20 },
  { TR_media_x24, TR_media_x24, a_off_media_24 },
  { TR_media_x28, TR_media_x28, a_off_media_28 },
  { TR_media_x2C, TR_media_x2C, a_off_media_2C },
  { TR_media_x30, TR_media_x30, a_off_media_30 },
  { TR_media_x34, TR_media_x34, a_off_media_34 },
  { TR_media_x38, TR_media_x38, a_media_x38 },
  { TR_media_x3C, TR_media_x3C, a_off_media_3C },
  { TR_media_x40, TR_media_x40, a_media_x40 },
  { TR_media_x44, TR_media_x44, a_off_media_44 },
  { TR_media_x48, TR_media_x48, a_off_media_48 },
  { TR_media_x4C, TR_media_x4C, a_off_media_4C },
  { TR_media_x50, TR_media_x50, a_off_media_50 },
  { TR_media_x54, TR_media_x54, a_media_x54 },
  { TR_media_x58, TR_media_x58, a_off_media_58 },
  /* setting */
  { TR_setting_AddRef, TR_setting_AddRef, a_setting_AddRef },
  { TR_setting_Release, TR_setting_Release, a_zm_svc_release },
  { TR_setting_x08, TR_setting_x08, a_off_set_08 },
  { TR_setting_x0C, TR_setting_x0C, a_off_set_0C },
  { TR_setting_x10, TR_setting_x10, a_off_set_10 },
  { TR_setting_x14, TR_setting_x14, a_off_set_14 },
  { TR_setting_x18, TR_setting_x18, a_setting_x18 },
  { TR_setting_x1C, TR_setting_x1C, a_setting_x1C },
  { TR_setting_x20, TR_setting_x20, a_setting_x20 },
  { TR_setting_x24, TR_setting_x24, a_setting_x24 },
  { TR_setting_x28, TR_setting_x28, a_off_set_28 },
  { TR_setting_x2C, TR_setting_x2C, a_off_set_2C },
  { TR_setting_x30, TR_setting_x30, a_off_set_30 },
  { TR_setting_x34, TR_setting_x34, a_off_set_34 },
  /* netmgr */
  { TR_netmgr_release, TR_netmgr_release, a_zm_svc_release },
  { TR_netmgr_x1C, TR_netmgr_x1C, a_zm_netmgr_x1C },
  /* tapi（精确项须先于区间） */
  { TR_tapi_x2C, TR_tapi_x2C, a_zm_tapi_x2C },
  { TR_tapi_x40, TR_tapi_x40, a_zm_tapi_x40 },
  { TR_tapi_release, TR_tapi_release, a_zm_svc_release },
  { TR_tapi_x00, TR_tapi_x50, d_tapi_stub },
  /* zip 区间 */
  { TR_zip_x00, TR_zip_x10, d_zip_stub },
  /* dll */
  { TR_dll_release, TR_dll_release, a_zm_svc_release },
  { TR_dll_init, TR_dll_init, a_zm_dll_init },
  { TR_dll_config, TR_dll_config, a_dll_config },
  { TR_dll_entry, TR_dll_entry, a_dll_entry },
  /* cbk */
  { TR_cbk_default, TR_cbk_default, a_zm_root_cbk_default },
};

/* 线性查表（首条命中即返回，故精确项须排在各自区间之前） */
static trap_fn trap_lookup(uint32_t addr) {
  for (size_t i = 0; i < sizeof(k_trap_table) / sizeof(k_trap_table[0]); i++) {
    if (addr >= k_trap_table[i].lo && addr <= k_trap_table[i].hi)
      return k_trap_table[i].fn;
  }
  return NULL;
}

/* 兜底：原 switch 的 default 分支（IBitmap 原生虚表 + 未接线槽告警），behavior 不变 */
static uint32_t trap_default(trap_ctx *c) {
  uint32_t trap_address = c->trap;
  if (trap_address >= TRAMP_BASE + 0x1700 &&
      trap_address < TRAMP_BASE + 0x1720) {
    uint32_t off = trap_address - (TRAMP_BASE + 0x1700);
    if (off == 0x10) { /* GetInfo(this, out)：RE memcpy(out, obj+8, 0x20) */
      if (!c->r0 || !c->r1)
        return (uint32_t)-4;
      uint8_t info[0x20];
      if (uc_mem_read(c->uc, c->r0 + 8, info, sizeof(info)) != UC_ERR_OK)
        return (uint32_t)-4;
      uc_mem_write(c->uc, c->r1, info, sizeof(info));
      return 0;
    } else if (off == 0x08) { /* SetTransColor(this, color) → 对象 +0x20 */
      if (c->r0)
        uc_mem_write(c->uc, c->r0 + 20, &c->r1, sizeof(c->r1));
      return 0;
    }
    return 0; /* AddRef / Release / 其余未知槽：真机亦返回 0 */
  }
  if (trap_address >= TRAMP_BASE && trap_address < TRAMP_BASE + TRAMP_SIZE) {
    uint32_t slot = trap_address - TRAMP_BASE;
    if (getenv("ZM_SHOW_TRACE") && (slot == 0x70 || slot == 0x12C)) {
      log_info("[SHIM细] slot=0x%X r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X",
               slot, c->r0, c->r1, c->r2, c->r3, c->lr);
      for (int i = 0; i < 10; i++)
        log_info("[SHIM细]   sp+%02d = %08X", i * 4,
                 uc_read32(c->uc, c->sp + (uint32_t)i * 4));
      if (slot == 0x70) {
        uint32_t dp = uc_read32(c->uc, c->r0 + 0);
        uint32_t ln = uc_read32(c->uc, c->r0 + 4);
        uint8_t bb[32];
        if (dp)
          uc_mem_read(c->uc, dp, bb, sizeof(bb));
        log_info("[SHIM细] CString@%08X dp=%08X len=%08X "
                 "data=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                 c->r0, dp, ln, bb[0], bb[1], bb[2], bb[3], bb[4], bb[5],
                 bb[6], bb[7], bb[8], bb[9], bb[10], bb[11], bb[12], bb[13],
                 bb[14], bb[15]);
      }
    }
    log_error("非法的外部调用: 0x%08X (SHIM槽+0x%X) r0=0x%X r1=0x%X "
              "r2=0x%X r3=0x%X sp[0]=0x%X sp[4]=0x%X lr=0x%X",
              trap_address, slot, c->r0, c->r1, c->r2, c->r3, uc_read32(c->uc, c->sp),
              uc_read32(c->uc, c->sp + 4), c->lr);
  } else {
    log_error("非法的外部调用: 0x%08" PRIx32, trap_address);
  }
  return 0;
}

void handle_trap(uc_engine *uc, uint32_t trap_address) {
  uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, sp = 0, lr = 0;
  /* 逐个检查寄存器读取结果，避免失败时使用未初始化值污染分发逻辑 */
  if (uc_reg_read(uc, UC_ARM_REG_R0, &r0) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_R1, &r1) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_R2, &r2) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_R3, &r3) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_SP, &sp) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_LR, &lr) != UC_ERR_OK) {
    log_error("Failed to read core registers at trap 0x%08" PRIx32,
              trap_address);
    return;
  }
  /* 参数打印：仅在开启反汇编调试（ZM_DISASM/g_disasm）时输出。
   * 曾经是无条件 printf，每次 trap 都刷 10 行，既拖慢模拟又污染 stdout。 */
  if (g_disasm) {
    for (int i = 0; i < 10; i++) {
      uint32_t v = getArg(uc, i);
      log_debug("trap[0x%X] arg%d = 0x%08X", trap_address - TRAMP_BASE, i, v);
    }
  }
  // log_debug("trap addr: %d, r0: %d, r1: %d, r2: %d, r3: %d, sp: %d, lr:
  // %d\n",
  //           trap_address, r0, r1, r2, r3, sp, lr);

  /* 寄存器 dump 只在反汇编调试下输出（原来每次都刷，既慢又淹没有效日志） */
  if (g_disasm)
    print_non_zero_registers(uc);

  /* 统计探针（ZM_STAT=1）：在真正分发前记一笔，供"哪个槽位被疯狂调用"分析 */
  zm_stat_trap(trap_address - TRAMP_BASE);

  hook_ctx_apply(uc); /* applet 上下文镜像（见 hook.c） */
  trap_ctx c = { uc, trap_address, r0, r1, r2, r3, lr, sp, false };
  trap_fn fn = trap_lookup(trap_address);
  uint32_t ret = fn ? fn(&c) : trap_default(&c);

  /* 带上槽号：排查"某个返回值被 applet 存进对象、之后当指针用"的场景时，
   * 只打 r0 的值分不清是哪个槽返回的。这里只改调试输出，不改任何行为。 */
  if (!c.handled) {
  if (trap_address >= TRAMP_BASE && trap_address < TRAMP_BASE + TRAMP_SIZE)
    log_debug("applet 调用外部[槽+0x%X] 返回 r0=0x%X (入参 r0=0x%X r1=0x%X "
              "r2=0x%X r3=0x%X lr=0x%X)",
              trap_address - TRAMP_BASE, ret, r0, r1, r2, r3, lr);
  else
    log_debug("applet 调用外部(0x%X) 返回 r0=0x%X (入参 r0=0x%X r1=0x%X "
              "r2=0x%X r3=0x%X lr=0x%X)",
              trap_address, ret, r0, r1, r2, r3, lr);

  uc_reg_write(uc, UC_ARM_REG_R0, &ret);

  uc_reg_write(uc, UC_ARM_REG_PC, &lr);
  }
}

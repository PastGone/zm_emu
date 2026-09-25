/**
 * @file trap_cbk_heap.c
 * @brief 客户机堆：applet 的 libc 接线 malloc/free/calloc + CBK 自管堆的搭建
 *
 * 【为什么单独一个文件】它和"陷阱分派"没有半点关系——这里做的是
 * "在客户机内存里手工搭一个堆管理器"，是 RE 出来的固件行为复现。
 * 放在分派文件里只会让读分派表的人先翻过一百多行无关代码。
 * 推导过程与历史尝试见 docs（代码里只留不变式与结论）。
 */

#include <stdlib.h> /* getenv */
#include <string.h> /* memcpy */

#include "../emu.h" /* g_ulibc_heap / g_heap_ptr / CBK* 常量 */
#include "../log/log.h"
#include "../ulibc/ulibc.h"          /* u_malloc / u_free / u_memset */
#include "../zmaee/core/zm_mem.h"    /* host_malloc（排查用 bump 分配器） */
#include "trap_internal.h"

/* ==========================================================================
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
 * ========================================================================== */

/** applet 的 malloc */
uint32_t applet_malloc(uc_engine *uc, uint32_t size) {
  if (g_ulibc_heap)
    return u_malloc(uc, size);
  return host_malloc(&g_heap_ptr, size); /* 排查用：只增不减 */
}

/** applet 的 free */
void applet_free(uc_engine *uc, uint32_t p) {
  if (g_ulibc_heap) {
    u_free(uc, p);
    return;
  }
  (void)uc;
  /* 排查模式：不回收，用于确认崩溃是否由内存回收引起 */
}

/** applet 的 calloc：分配并清零（清零在客户机侧完成，不开宿主临时缓冲） */
uint32_t applet_calloc(uc_engine *uc, uint32_t n, uint32_t size) {
  uint32_t p = applet_malloc(uc, n * size);
  if (p)
    u_memset(uc, p, 0, n * size);
  return p;
}

/* ==========================================================================
 * CBK 自管堆（create_cbk 对象内嵌的分配器）
 * ==========================================================================
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
 * 这里按上面的格式建一个空堆：一整块空闲块，让 0x175B8 自己去切。只做一次。
 *
 * 不变式（改这个文件前先读）：
 *   1) 管理器 +8（"区末/分配上限"）必须正好是**跳板页首** —— 它同时是
 *      "分配上限"和"可调用地址"两个身份（见 emu_mem_regions.h 的 CBK_STUB_BASE）。
 *      旧实验把 +8 填 TRAMP_BASE+0x3C ✗：低于区起点 → 上限 < 起点 → 分配全废。
 *   2) 管理器 +4 是首块真值、+8 是区末真值，**不能被桩覆盖**；
 *      其余方法槽（+0xC..+0x3C）全是"applet 会当方法调"的槽 → 必须填有效跳板。
 *   3) 每个桶的 +8（节点步长）必须是 4/8/0x10/0x20，全 0 会让切出来的节点
 *      只有 8 字节 → applet 要 4/8/0x10/0x20 时立刻溢出。
 * ========================================================================== */
void cbk_heap_init_once(uc_engine *uc) {
  static int done = 0;
  if (done)
    return;
  done = 1;

  /* 0x15DDC 每次“补桶”会向管理器要 0x8000(32KB)（RE：0x15E1C `mov r1,#128,#28`
   * = 0x8000），128KB 几个桶就见底 → 分配返回 0 → applet 拿到 NULL 后又去
   * Release(0) → 崩（实测 pc=0x173D4 / 0x52069AD8）。给足 1MB。 */
  /* 堆大小 = 整个 CBKHEAP 区（除开头的分配器头 0x50）——
   * 上限由 `[管理器+8]` 决定，而那个值现在必须正好是**跳板页首**
   * （见 emu_mem_regions.h 的 CBK_STUB_BASE 说明：它要同时当"分配上限"和
   * "可调用地址"）。所以这里不再提供变小堆的开关：堆变小 → 上限低于页首
   * → applet 那个 `[[CBK+0x4C]] → [+8] → blx` 就跳不到桩上了。
   * 1MB 对自管堆足够（原来给 0x1F000 也够用，只是那会儿管理器还在区尾）。 */
  uint32_t HEAP_BYTES = CBKHEAP_SIZE - 0x50u;
  /* 布局：专用区开头放分配器(0x40)；管理器放**区外**的跳板页（见下）。
   * ★ 既不能向 applet 自己的堆要内存（`applet_malloc`/u_malloc：会把 applet
   * 堆起点整体后移，00000001 立刻崩），也不能放在 blob 里（会与 applet 的
   * 静态数据/我们自己造的堆对象撞车）。用独立的 CBKHEAP 映射区（见
   * emu_mem_regions.h）。 */
  /* 管理器放到**区外**的跳板页里（页首 8 字节是桩代码，管理器在 CBK_MGR_OFF）：
   *   00000710 家族：`[[CBK_OBJ+0x4C]] → [+0x30] → blx`
   *   0000050b 家族：`[[CBK+0x4C]] → [+8] → blx`（本次新增支持）
   * 即它们把 [CBK+0x4C] 当"带虚表的对象"用；而 00000502 那条路（0x15DDC/0x175B8）
   * 又把 [CBK+0x4C] 的 [+0] 当**堆管理器**读，管理器 = { +4 首块, +8 区末 }。
   * ⇒ [+0] 仍指管理器，但管理器搬到区外，于是：
   *     · +0x30 填中性桩（00000710 家族要的）
   *     · +8   = "区末" = **跳板页首** CBK_STUB_BASE —— 页首恰好也是一段
   *       "返回 0"的桩，**数值上界**与**可调用地址**两个要求同时满足 ✓
   *       （旧实验把 +8 填 TRAMP_BASE+0x3C：低于区起点 → 上限 < 起点 → 分配全废 ✗）*/
  uint32_t mgr = CBK_STUB_BASE + CBK_MGR_OFF;
  uint32_t al = CBKHEAP_BASE + 0x10u;     /* 分配器 0x40 字节 */
  uint32_t region = CBKHEAP_BASE + 0x50u; /* 堆区起点 */
  if (HEAP_BYTES < 0x8000u) {
    log_warn("cbk 堆初始化失败：区太小(%u)", HEAP_BYTES);
    return;
  }
  /* 管理器的"方法槽"**全部**填成有效跳板。
   *
   * 这一族 applet 把 [CBK+0x4C]（分配器）当"带虚表的对象"用，并且**调用的槽偏移
   * 各不相同**（实测一路踩过来）：
   *   00000710 家族：`r2 = [[CBK+0x4C]+0x30]; blx r2`（结果按字节当 bool）
   *   0000050b 家族：`r3 = [[CBK+0x4C]+0x8];  blx r3`（我们已把它做成跳板页首 ✓）
   *                   之后又 `r2 = [管理器+0x20]; blx r2`   ← 本次补上
   * 只填某一个槽就只能过一层，下一层又跳飞 ✗。所以这里把**除 +4/+8 之外**的
   * 每个字都填成跳板页首（CBK_STUB_BASE，那上面有"返回中性假对象"的桩 ✓）：
   *   +4 = 首块、+8 = 区末 —— 分配器（0x15DDC/0x175B8）要的真值，**不能动** ✗
   *   其余（+0xC..+0x3C 等）都是 applet 会当方法调的槽 → 填桩 ✓
   * 注意 +0 一直是 0（分配器自己写）；这里从 +0xC 开始填到 +0x40。 */
  {
    uint32_t stub = CBK_STUB_BASE;
    for (uint32_t o = 0xCu; o < 0x40u; o += 4) {
      if (o == 4 || o == 8)
        continue; /* 首块 / 区末：真值，跳过 */
      uc_mem_write(uc, mgr + o, &stub, 4);
    }
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

  /* 管理器 { +4 首块, +8 区末 }；分配器 [0] = 管理器。
   * 区末**固定**填跳板页首（不是 region+HEAP_BYTES 的旧算法）：两者在默认配置下
   * 数值相同（HEAP_BYTES 已取满），但显式填死的意义是——万一有人改 HEAP_BYTES，
   * 那个"可调用地址"也不会被破坏 ✓。 */
  {
    uint32_t first = region;
    /* 区末 = **CBK 陷阱窗口**（在跳板页里、堆区之上 ⇒ 两个身份仍然同时成立 ✓）：
     *   · 对分配器：它是"分配上限"，数值合法 ✓
     *   · 对这一族 applet：它是"打开资源文件的工厂"入口 —— 一个**我们能接手处理的
     *     陷阱**（见 zm_cbk_file.h），而不是以前那段"只会返回假对象"的桩 ✗
     *     （那正是 malloc(16MB) → NULL → 画到映射外的根因）。 */
    uint32_t end = CBK_TRAP_BASE;
    uc_mem_write(uc, mgr + 4, &first, 4);
    uc_mem_write(uc, mgr + 8, &end, 4);
    uc_mem_write(uc, al, &mgr, 4);
  }
  if (!getenv("ZM_NO_CBKPTR"))
    uc_mem_write(uc, CBK_OBJ + 0x4C, &al, 4);
  log_info("create_cbk 自管堆已建：分配器=0x%X 管理器=0x%X 区=0x%X..0x%X"
           "（区末=跳板页 0x%X，管理器 +8 可调用 ✓）",
           al, mgr, region, region + HEAP_BYTES, CBK_STUB_BASE);
}

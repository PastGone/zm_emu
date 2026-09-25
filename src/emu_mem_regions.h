#ifndef EMU_MEM_REGIONS_H
#define EMU_MEM_REGIONS_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * 顶层内存布局（纯地址算术）
 * ============================================================ */
static constexpr uint32_t ONE_MB = 0x100000U;
static constexpr uint32_t HALF_MB = 0x80000U;

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
static constexpr uint32_t BLOB_BASE = 0x0U;
static constexpr uint32_t BLOB_SIZE = 1 * ONE_MB;

static constexpr uint32_t STACK_BASE = BLOB_BASE + BLOB_SIZE;
static constexpr uint32_t STACK_SIZE = 1 * HALF_MB;
static constexpr uint32_t STACK_TOP = STACK_BASE + STACK_SIZE;
/* 栈顶 red-zone：部分 applet（如 00000459）的裸函数 sub_698 用 POP 弹掉的
 * 寄存器数比调用方 sub_3BC 用 PUSH 压入的多 2 个，会越界读 STACK_TOP 之上约
 * 8 字节。初始 SP 不顶在 STACK_TOP，而是下沉 STACK_REDZONE，让这段越界读落
 * 在映射区内（不影响 HEAP/SHIM 布局，普通 applet 也仅是多了点头部余量）。 */
static constexpr uint32_t STACK_REDZONE = 0x200;

static constexpr uint32_t HEAP_BASE = STACK_TOP + ONE_MB / 8;
static constexpr uint32_t HEAP_SIZE = 6 * ONE_MB;
static constexpr uint32_t HEAP_END = HEAP_BASE + HEAP_SIZE;

static constexpr uint32_t SHIM_FT_BASE = HEAP_END; /* 函数表 */
static constexpr uint32_t SHIM_FT_SIZE = HALF_MB / 2;

static constexpr uint32_t SHIM_BASE = SHIM_FT_BASE;
/* VT 32K + DATA 32K + OBJ 64K + POOL 64K + PIXEL 0x680000 */
static constexpr uint32_t SHIM_SIZE = 0x6B0000U;

static constexpr uint32_t TRAMP_BASE = SHIM_BASE + SHIM_SIZE;
static constexpr uint32_t TRAMP_SIZE = 1 * HALF_MB;

/* CBK 自管堆（applet 通过 [CBK_OBJ+0x4C] 自己管理的那个分配器）的专用区。
 *
 * ★ 位置必须独立：既不能向 applet 自己的堆要（`applet_malloc`/u_malloc 会把
 * applet 堆起点整体后移，00000001 立刻崩），也不能放在 blob 区里（会与
 * applet 的静态数据/我们自己造的堆对象撞车）。
 * 放在 TRAMP 之后，1MB。
 *
 * ★ 大小必须够：RE 0x15DDC 每次"补桶"都要向堆管理器要 0x8000(32KB)
 * （0x15E1C `mov r1,#128,#28` = 0x8000），而桶有 4 个（步长 4/8/0x10/0x20）
 * → 4*32KB = 128KB。给 128KB 时第 4 个桶（0x1c 这类请求用）必然补桶失败 →
 * 0x191B4 的 0x1c 分配返回 0 → 类表条目为 NULL → 对 NULL 做相对虚表派发 →
 * 崩在 0x80E291E8。给 1MB。 */
static constexpr uint32_t CBKHEAP_BASE = TRAMP_BASE + TRAMP_SIZE;
static constexpr uint32_t CBKHEAP_SIZE = 1 * ONE_MB;

/* CBK 堆区**之上**的一个 4KB "跳板页"（见 trap.c 的 cbk_heap_init_once）。
 *
 * 为什么需要：有一族 applet 把 `[CBK_OBJ+0x4C]` 当"带虚表的对象"用——
 *   r0 = [CBK+0x4C]; r1 = 1; r2 = [[r0]+0x30]; blx r2      （00000710 家族）
 *   r0 = [r0];      r3 = [[r0]+0x8];  blx r3                （0000050b 家族）
 * 而另一条路（00000502 的分配器 0x15DDC/0x175B8）要求
 *   [分配器+0] = 堆管理器，管理器 { +4 首块, +8 区末 }（+8 是**分配上限**）
 * 两者在"管理器 +8"上硬冲突：一个要它是数值上界，一个要它是函数指针。
 *
 * 解法：把**区末定成这个页的首地址**，并在页首放一段"返回 0"的桩 ✓
 * —— 它同时满足"分配上限"（堆区正好到页首为止，下面全是已映射内存 ✓）
 * 和"可调用"（blx 过去就是 mov r0,#0; bx lr ✓）。
 * 注意：旧实验把 +8 填成 TRAMP_BASE+0x3C（**低于**堆区起点）→ 分配上限 < 区起点
 * → 分配全失败（00000502 退到 0x7B080C、000007xx 全退 0x18 ✗）。位置**必须在上方**。 */
static constexpr uint32_t CBK_STUB_BASE = CBKHEAP_BASE + CBKHEAP_SIZE;
static constexpr uint32_t CBK_STUB_SIZE = 0x1000U;
/* 管理器搬到跳板页里（页首是桩代码，管理器放 +0x100） */
static constexpr uint32_t CBK_MGR_OFF = 0x100U;

/* 跳板页里的**宿主陷阱窗口**（见 trap.c 的"CBK 文件对象陷阱"）。
 *
 * 这一族 applet（0000050b/00001b63 等）把 [CBK_OBJ+0x4C]（分配器）当"带虚表的对象"，
 * 并且把它的 [+0]→[+8] 当**打开资源文件**的工厂用：先拼 "<目录>\res\gameN.ypak"
 * （实测字符串 ✓），再把路径传进来，拿回一个"文件对象"，随后
 *     size = obj->vt[0x24]();  buf = malloc(size);  obj->vt[0x08](buf, size);
 * 真机那里是文件/资源服务；我们以前给的是"只会返回假对象的桩" ✗ → size 拿到的是
 * **指针**（0xFD0300）→ malloc(16MB) 失败 → NULL → 后面拿尺寸当指针 → 崩。
 * 现在 [管理器+8] 指向这里的第一个陷阱，由宿主按**真文件**办事（见 zm_cbk_file.c）。
 *
 * 地址仍在堆区之上 ⇒ 它同时继续充当分配器的"区末上界"（旧要求不能丢 ✗）。 */
static constexpr uint32_t CBK_TRAP_BASE = CBK_STUB_BASE + 0x40U;
static constexpr uint32_t CBK_TRAP_SIZE = 0x40U;

/* 文件对象的假虚表与包装对象池（都在跳板页里；页内布局见 emu.c）：
 *   +0x240 假虚表（给"管理器方法槽"返回的那个假对象用）
 *   +0x600 文件对象虚表：+0x04 release / +0x08 read / +0x0C write /
 *                        +0x20 seek / +0x24 **文件大小**
 *   +0x800 包装对象池（4 × 0x20） */
static constexpr uint32_t CBK_FILE_VT = CBK_STUB_BASE + 0x600U;
static constexpr uint32_t CBK_FILE_OBJ = CBK_STUB_BASE + 0x800U;
static constexpr uint32_t CBK_FILE_OBJ_STRIDE = 0x20U;
static constexpr uint32_t CBK_FILE_MAX = 4U;

/* 跳板页之后的**尾部暂存区**（256KB，清零）。
 *
 * 为什么需要：那一族 applet 拿到"区末"（[管理器+8] = 跳板页首 0xFD0000）之后，
 * 会把它当"堆后面的空白区"用来**清缓冲** —— 实测 0000050b 就是从 0xFD0000+0x1000
 * 开始 `strh` 逐 2 字节写 0、一直写到 0xFD3558（约 9.5KB），写到未映射区就 err=7 ✗。
 * 真机上那片是它自己的可写内存；我们在这里补一块够大的垫子，让它清得过、
 * 又**不覆盖**跳板页里的桩/管理器（垫子从 CBK_TAIL_BASE = 跳板页之后开始 ✓）。 */
static constexpr uint32_t CBK_TAIL_BASE = CBK_STUB_BASE + CBK_STUB_SIZE;
/* 【先粗后细】先给 16MB 兜底，观察这族 applet 到底要用多大、怎么用这块内存
 * （实测它每加一块垫子就往后要下一块：0xFD1000 → 0x1011000，说明"区末"这个
 * 值在它眼里可能不是"区末"而是"可用空间起点/缓冲基址"）。等看清用法再收回成
 * 正确语义的实现，见 CBK_STUB_BASE 那段注释。 */
static constexpr uint32_t CBK_TAIL_SIZE = 16 * ONE_MB;

static constexpr uint32_t ROOT_SLOT_OFF = 0x180U;
static constexpr uint32_t APPLET_ENTRY_OFF = 0x188U;
static constexpr uint32_t APPLET_ENTRY_POINT = BLOB_BASE + APPLET_ENTRY_OFF;

/* ============================================================
 * SHIM 内部分区
 *
 *   [ VT ][ DATA ][ OBJ ][ POOL ][ PIXEL ][ reserved ]
 *   0   32K    64K   128K   192K   768K       1M
 * ============================================================ */
static constexpr uint32_t SHIM_VT_BASE = SHIM_BASE + 0x00000U; /* 32KB */
static constexpr uint32_t SHIM_VT_SIZE = 0x08000U;

static constexpr uint32_t SHIM_DATA_BASE = SHIM_BASE + 0x08000U; /* 32KB */
static constexpr uint32_t SHIM_DATA_SIZE = 0x08000U;

static constexpr uint32_t SHIM_OBJ_BASE = SHIM_BASE + 0x10000U; /* 64KB */
static constexpr uint32_t SHIM_OBJ_SIZE = 0x10000U;

static constexpr uint32_t SHIM_POOL_BASE = SHIM_BASE + 0x20000U; /* 64KB */
static constexpr uint32_t SHIM_POOL_SIZE = 0x10000U;

static constexpr uint32_t SHIM_PIXEL_BASE = SHIM_BASE + 0x30000U; /* 0x680000 */
static constexpr uint32_t SHIM_PIXEL_SIZE = 0x680000U;

#endif /* EMU_MEM_REGIONS_H */

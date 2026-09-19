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

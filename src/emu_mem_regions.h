#ifndef EMU_MEM_REGIONS_H
#define EMU_MEM_REGIONS_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * 顶层内存布局（纯地址算术）
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
/* VT 32K + DATA 32K + OBJ 64K + POOL 64K + PIXEL 0x680000 */
#define SHIM_SIZE (0x6B0000U)

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

#define SHIM_PIXEL_BASE (SHIM_BASE + 0x30000U) /* 0x680000 */
#define SHIM_PIXEL_SIZE (0x680000U)

#endif /* EMU_MEM_REGIONS_H */

#include "./hook.h"
#include "./log/log.h"
#include "./tool/disasm_log.h"
#include "./trap.h"
#include <inttypes.h>
#include <stdio.h>

/* 兜底次数上限，防止 applet 进入"疯狂访问非法内存"的死循环时刷屏 */
#define MAX_RECOVER 4096

static uint32_t s_unmapped_cnt = 0;
static uint32_t s_invalid_cnt = 0;

/* 合法执行区：blob(0x80000,4MB) / SHIM+TRAMP(0x10700000,1MB) / 零页。
 * 其余地址视为"野地址执行"，hook_insn_invalid 会回退到 LR。 */
static bool pc_is_legal(uint32_t addr) {
  if (addr < 0x1000) /* 零页：NULLCALL 兜底 */
    return true;
  if (addr >= BLOB_BASE && addr < BLOB_BASE + BLOB_SIZE)
    return true;
  if (addr >= SHIM_BASE && addr < TRAMP_BASE + TRAMP_SIZE)
    return true;
  return false;
}

/* 把 addr 所在的 4KB 页映射出来，并整页填成 "立刻返回" 指令。
 * ARM 态填 0xE12FFF1E（bx lr），Thumb 态填 0x4770（bx lr）。
 * 这样任何跳进这一页的"未建模函数调用"都会立即回到调用者，
 * 既不会崩溃，也不会让 Unicorn 因为页仍未映射而抛 UC_ERR_MAP。 */
static bool map_return_stub(uc_engine *uc, uint64_t addr) {
  uint64_t page = addr & ~(uint64_t)0xFFF;
  if (!zm_emu_map_on_demand(page))
    return false;

  uint8_t buf[0x1000];
  uint32_t cpsr = 0;
  uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
  if (cpsr & (1u << 5)) { /* T 位：Thumb 态 */
    for (size_t i = 0; i < sizeof(buf); i += 2) {
      buf[i] = 0x70;
      buf[i + 1] = 0x47;
    }
  } else {
    for (size_t i = 0; i < sizeof(buf); i += 4) {
      buf[i] = 0x1E;
      buf[i + 1] = 0xFF;
      buf[i + 2] = 0x2F;
      buf[i + 3] = 0xE1;
    }
  }
  return uc_mem_write(uc, page, buf, sizeof(buf)) == UC_ERR_OK;
}

void hook_code(uc_engine *uc, uint64_t address, uint32_t size,
               void *user_data) {
  if (g_disasm) {
    disassemble_and_log(uc, address, size);
  }

  if (address >= TRAMP_BASE && address < TRAMP_BASE + TRAMP_SIZE) {
    handle_trap(uc, (uint32_t)address);
  }
}

void hook_shim_mem(uc_engine *uc, uc_mem_type type, uint64_t address, int size,
                   int64_t value, void *user_data) {
  if (type == UC_MEM_READ) {
    log_trace("[MEM] 读 0x%08" PRIx64 " (%d 字节) -> %s", address, size,
              zm_trap_name((uint32_t)address));
  } else if (type == UC_MEM_WRITE) {
    log_trace("[MEM] 写 0x%08" PRIx64 " (%d 字节) = 0x%" PRIx64 " -> %s",
              address, size, (uint64_t)value, zm_trap_name((uint32_t)address));
  }
}

bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address,
                       int size, int64_t value, void *user_data) {
  const char *kind = "访问";
  switch (type) {
  case UC_MEM_READ_UNMAPPED:
  case UC_MEM_READ_PROT:
    kind = "读";
    break;
  case UC_MEM_WRITE_UNMAPPED:
  case UC_MEM_WRITE_PROT:
    kind = "写";
    break;
  case UC_MEM_FETCH_UNMAPPED:
  case UC_MEM_FETCH_PROT:
    kind = "取指";
    break;
  default:
    break;
  }

  /* 取指到非法地址：通常是 applet 通过 vtable / 函数指针调用了一个
   * 我们并未建模的框架函数（指针是 0 或未初始化的野指针）。
   * 与其直接判崩溃，不如把这次"调用"当成返回 0 跳过，让 applet 继续跑下去。
   *
   * 【坑】不能只把 PC 改成 LR 就 return true：Unicorn 处理完 hook 后会
   * 重试这条取指，发现该页仍未映射，就把 uc_emu_start 的返回值置成
   * UC_ERR_MAP，整个模拟照样异常退出。正确做法是真的把这一页映射出来，
   * 并整页填上 "bx lr"，让这次调用自然地立刻返回。
   * 真正跑飞的情况由指令数上限（ZM_INSN_CAP）兜底，不会卡死。 */
  if (type == UC_MEM_FETCH_UNMAPPED || type == UC_MEM_FETCH_PROT) {
    uint32_t lr = 0;
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    if (++s_unmapped_cnt <= 32) {
      log_warn("[MEM] %s 未映射地址 0x%08" PRIx64
               " (LR=0x%08X)，铺 bx lr 桩，按调用返回 0",
               kind, address, lr);
    } else if (s_unmapped_cnt == 33) {
      log_warn("[MEM] 未映射访问过多，后续不再逐条打印");
    }

    if (s_unmapped_cnt > MAX_RECOVER) {
      log_error("[MEM] 未映射访问超过 %d 次，判定 applet 已跑飞，结束模拟",
                MAX_RECOVER);
      g_stop_requested = 1;
      uc_emu_stop(uc);
      return true;
    }

    if (map_return_stub(uc, address)) {
      uint32_t r0 = 0;
      uc_reg_write(uc, UC_ARM_REG_R0, &r0);
      return true;
    }

    log_error("[MEM] 铺桩失败 @0x%08" PRIx64 "，结束模拟", address);
    g_stop_requested = 1;
    uc_emu_stop(uc);
    return true;
  }

  if (++s_unmapped_cnt <= 32) {
    uint32_t pc = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    log_warn("[MEM] %s 未映射地址 0x%08" PRIx64 " (%d 字节)，按需补页 (PC=0x%08X)",
             kind, address, size, pc);
  } else if (s_unmapped_cnt == 33) {
    log_warn("[MEM] 未映射访问过多，后续不再逐条打印");
  }

  if (s_unmapped_cnt > MAX_RECOVER) {
    log_error("[MEM] 未映射访问超过 %d 次，判定 applet 已跑飞，结束模拟",
              MAX_RECOVER);
    g_stop_requested = 1;
    uc_emu_stop(uc);
    return true;
  }

  /* 补一页零页让读写落地，返回 true 表示"已处理"，Unicorn 会重试该指令 */
  if (zm_emu_map_on_demand(address)) {
    return true;
  }

  log_error("[MEM] 补页失败 @0x%08" PRIx64 "，结束模拟", address);
  g_stop_requested = 1;
  uc_emu_stop(uc);
  return true;
}

/* 空函数指针调用（虚表未建模的子对象）兜底：
 * applet 通过 vtable 方法调用框架子对象，而该子对象在我们的对象模型里是 0，
 * 于是 blx 0 落到零页。把这种"在零页执行"当成"该调用返回 0"，直接回到调用者，
 * 既不崩溃、也不阻断后续的事件循环。 */
static uint32_t s_nullcall_cnt = 0;

void hook_null_call(uc_engine *uc, uint64_t address, uint32_t size,
                    void *user_data) {
  (void)size;
  (void)user_data;
  if (address >= (uint64_t)0x1000)
    return; /* 仅对零页（空指针区域）生效 */

  if (++s_nullcall_cnt <= 32) {
    uint32_t lr = 0;
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    log_warn("[NULLCALL] 空函数指针调用 @0x%08" PRIx64 "，按返回 0 跳过 (LR=0x%08X)",
             address, lr);
  }
  uint32_t r0 = 0;
  uint32_t lr = 0;
  uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uc_reg_write(uc, UC_ARM_REG_PC, &lr);
}

bool hook_insn_invalid(uc_engine *uc, void *user_data) {
  uint32_t pc = 0;
  uc_reg_read(uc, UC_ARM_REG_PC, &pc);

  if (!pc_is_legal(pc)) {
    /* 野执行（如 0x70706132 = "ppa2"）：回退到 LR 兜住，
     * 避免在垃圾数据上跑飞耗尽指令上限。 */
    uint32_t lr = 0;
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    if (pc_is_legal(lr)) {
      if (++s_invalid_cnt <= 16)
        log_warn("[CPU] 野地址执行 @0x%08X，回退到 LR=0x%08X", pc, lr);
      uc_reg_write(uc, UC_ARM_REG_PC, &lr);
      return true;
    }
    if (++s_invalid_cnt <= 16)
      log_warn("[CPU] 野地址执行 @0x%08X 且 LR=0x%08X 也非法，跳过 4 字节",
               pc, lr);
  } else if (++s_invalid_cnt <= 16) {
    log_warn("[CPU] 非法指令 @0x%08X，跳过 4 字节继续", pc);
  }

  if (s_invalid_cnt > 256) {
    log_error("[CPU] 非法指令过多（%u 次），结束模拟", s_invalid_cnt);
    g_stop_requested = 1;
    uc_emu_stop(uc);
    return true;
  }

  /* 当成 NOP：PC 前进一条 ARM 指令。返回 true 表示已处理。 */
  pc += 4;
  uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  return true;
}

#include "./hook.h"
#include "./log/log.h"
#include "./tool/disasm_log.h"
#include "./trap.h"
#include <inttypes.h>
#include <stdio.h>

void hook_code(uc_engine *uc, uint64_t address, uint32_t size,
               void *user_data) {
  // 这个地方好像不对,因为啥来ARM32的规定，pc=address+8

  if (g_disasm) {
    disassemble_and_log(uc, address, size);
  }

  if (address >= TRAMP_BASE && address < TRAMP_BASE + TRAMP_SIZE) {
    uint32_t r0, r1, r2, r3, sp, lr;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);

    log_debug("trap pc: %d, r0: %d, r1: %d, r2: %d, r3: %d, sp: %d, lr: %d\n",
              address, r0, r1, r2, r3, sp, lr);

    handle_trap(uc, address, r0, r1, r2, r3, sp, lr);

    if (g_trap_pause) {
      log_info("按回车键继续...");
      scanf("%*c");
    }
  }
}

void hook_shim_mem(uc_engine *uc, uc_mem_type type, uint64_t address, int size,
                   int64_t value, void *user_data) {
  if (type == UC_MEM_READ) {
    log_debug("[HOOK] 读取 地址:0x%016lx 大小:%d\n", address, size);
  } else if (type == UC_MEM_WRITE) {
    log_debug("[HOOK] 写入 地址:0x%016lx 大小:%d 值:0x%016lx\n", address, size,
              value);
  } else {
    log_debug("[HOOK] 其他内存操作 (type=%d)\n", type);
  }
}

bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address,
                       int size, int64_t value, void *user_data) {
  log_warn("  !! MEM unmapped @0x%" PRIx64 " size=%d\n", address, size);
  return false;
}
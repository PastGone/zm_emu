#include "./hook.h"
#include "./log/log.h"
#include "./trap.h"
#include <inttypes.h>
#include <stdio.h>

void hook_code(uc_engine *uc, uint64_t address, uint32_t size,
               void *user_data) {
  uint32_t pc = address;

  if (g_disasm) {
    uc_mem_read(uc, pc, code, size);

    count = cs_disasm(cs_handle, code, size, pc, 0, &insn);
    if (count > 0) {
      char line[256];
      for (size_t i = 0; i < count; i++) {
        int offset =
            snprintf(line, sizeof(line), "0x%08" PRIx64 ":  ", insn[i].address);

        for (int j = 0; j < 4; j++) {
          if (j < insn[i].size) {
            offset += snprintf(line + offset, sizeof(line) - offset, "%02x ",
                               insn[i].bytes[j]);
          } else {
            offset += snprintf(line + offset, sizeof(line) - offset, "   ");
          }
        }

        snprintf(line + offset, sizeof(line) - offset, "%-8s %s",
                 insn[i].mnemonic, insn[i].op_str);

        log_info("%s\n", line);
      }
      cs_free(insn, count);

    } else {
      fprintf(stderr, "Disassembly failed\n");
    }
  }

  if (pc >= TRAMP_BASE && pc < TRAMP_BASE + TRAMP_SIZE) {
    uint32_t r0, r1, r2, r3, sp, lr;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);

    log_info("trap pc: %d, r0: %d, r1: %d, r2: %d, r3: %d, sp: %d, lr: %d\n",
             pc, r0, r1, r2, r3, sp, lr);

    handle_trap(uc, pc, r0, r1, r2, r3, sp, lr);

    if (g_trap_pause) {
      log_info("按回车键继续...");
      scanf("%*c");
    }
  }
}

void hook_shim_mem(uc_engine *uc, uc_mem_type type, uint64_t address, int size,
                   int64_t value, void *user_data) {
  if (type == UC_MEM_READ) {
    log_info("[HOOK] 读取 地址:0x%016lx 大小:%d\n", address, size);
  } else if (type == UC_MEM_WRITE) {
    log_info("[HOOK] 写入 地址:0x%016lx 大小:%d 值:0x%016lx\n", address, size,
             value);
  } else {
    log_info("[HOOK] 其他内存操作 (type=%d)\n", type);
  }
}

bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address,
                       int size, int64_t value, void *user_data) {
  log_warn("  !! MEM unmapped @0x%" PRIx64 " size=%d\n", address, size);
  return false;
}
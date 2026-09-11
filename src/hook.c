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
    handle_trap(uc, address);
  }
}

void hook_shim_mem(uc_engine *uc, uc_mem_type type, uint64_t address, int size,
                   int64_t value, void *user_data) {
  if (type == UC_MEM_READ) {
    log_debug("[HOOK] 从 0x%016lx 地址处读取大小为:%d的数据，值为:0x%016lx\n",
              address, size, value);

  } else if (type == UC_MEM_WRITE) {
    log_debug("[HOOK] 向 0x%016lx 地址处写入大小为:%d的数据，值为:0x%016lx\n",
              address, size, value);
  } else {
    log_debug("[HOOK] 其他内存操作 (type=%d)\n", type);
  }
}

bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address,
                       int size, int64_t value, void *user_data) {
  /* 打印崩溃时上下文：PC（正执行指令）、LR（返回地址）、R0-R3（调用参数），
   * 用于判断是哪个 stub 返回 0 被当函数指针/对象解引用。 */
  uint32_t pc = 0, lr = 0, r0 = 0, r1 = 0, r2 = 0, r3 = 0;
  uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  uc_reg_read(uc, UC_ARM_REG_R1, &r1);
  uc_reg_read(uc, UC_ARM_REG_R2, &r2);
  uc_reg_read(uc, UC_ARM_REG_R3, &r3);
  /* 调试：dump applet 固定区 0x180 的 ROOT 指针与 ROOT 表前 16 字节，
   * 定位 sub_84410 读 [ROOT+8] 得到 0 的原因。 */
  uint32_t root_slot = 0, root0 = 0, root8 = 0, rootc = 0;
  uc_mem_read(uc, BLOB_BASE + ROOT_SLOT_OFF, &root_slot, 4);
  uc_mem_read(uc, root_slot + 0, &root0, 4);
  uc_mem_read(uc, root_slot + 8, &root8, 4);
  uc_mem_read(uc, root_slot + 0xC, &rootc, 4);
  log_warn("  !! MEM unmapped @0x%" PRIx64 " size=%d"
           "  PC=0x%X LR=0x%X R0=0x%X R1=0x%X R2=0x%X R3=0x%X"
           "  [0x180]=0x%X [ROOT]=0x%X [ROOT+8]=0x%X [ROOT+C]=0x%X\n",
           address, size, pc, lr, r0, r1, r2, r3, root_slot, root0, root8,
           rootc);
  return false;
}
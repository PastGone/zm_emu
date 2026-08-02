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
  log_warn("  !! MEM unmapped @0x%" PRIx64 " size=%d\n", address, size);
  return false;
}
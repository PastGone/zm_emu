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

/* ZM_CAPTURE 时，对每个唯一 LR 记录首次现场，用于反推 null 指针来源。
 * 生产环境（无该变量）完全零开销。 */
#define NC_OBS_CAP 32
static uint32_t s_nc_lr[NC_OBS_CAP];
static int s_nc_n = 0;

void hook_null_call(uc_engine *uc, uint64_t address, uint32_t size,
                    void *user_data) {
  (void)size;
  (void)user_data;
  if (address >= (uint64_t)0x1000)
    return; /* 仅对零页（空指针区域）生效 */

  uint32_t lr = 0;
  uc_reg_read(uc, UC_ARM_REG_LR, &lr);

  if (++s_nullcall_cnt <= 32) {
    log_warn("[NULLCALL] 空函数指针调用 @0x%08" PRIx64 "，按返回 0 跳过 (LR=0x%08X)",
             address, lr);
  }

  if (getenv("ZM_CAPTURE")) {
    int found = -1;
    for (int i = 0; i < s_nc_n; i++)
      if (s_nc_lr[i] == lr) {
        found = i;
        break;
      }
    if (found < 0 && s_nc_n < NC_OBS_CAP) {
      found = s_nc_n++;
      s_nc_lr[found] = lr;
      uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, sp = 0;
      uint32_t r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0,
               r11 = 0, r12 = 0;
      uc_reg_read(uc, UC_ARM_REG_R0, &r0);
      uc_reg_read(uc, UC_ARM_REG_R1, &r1);
      uc_reg_read(uc, UC_ARM_REG_R2, &r2);
      uc_reg_read(uc, UC_ARM_REG_R3, &r3);
      uc_reg_read(uc, UC_ARM_REG_R4, &r4);
      uc_reg_read(uc, UC_ARM_REG_R5, &r5);
      uc_reg_read(uc, UC_ARM_REG_R6, &r6);
      uc_reg_read(uc, UC_ARM_REG_R7, &r7);
      uc_reg_read(uc, UC_ARM_REG_R8, &r8);
      uc_reg_read(uc, UC_ARM_REG_R9, &r9);
      uc_reg_read(uc, UC_ARM_REG_R10, &r10);
      uc_reg_read(uc, UC_ARM_REG_R11, &r11);
      uc_reg_read(uc, UC_ARM_REG_R12, &r12);
      uc_reg_read(uc, UC_ARM_REG_SP, &sp);
      log_info("[NULLCALL-OBS] LR=0x%08X r0=0x%X r1=0x%X r2=0x%X r3=0x%X "
               "r4=0x%X r5=0x%X r6=0x%X r7=0x%X r8=0x%X r9=0x%X r10=0x%X "
               "r11=0x%X r12=0x%X sp=0x%X",
               lr, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, sp);
      /* dump 候选 this 对象（r4..r7）的前 32 字节（含 vtable 与空方法槽），
       * 用于识别是哪个框架对象的 vtable 槽为空。 */
      uint8_t obj[32];
      uint32_t cand[4] = {r4, r5, r6, r7};
      const char *cn[4] = {"r4", "r5", "r6", "r7"};
      for (int ci = 0; ci < 4; ci++) {
        uint32_t base = cand[ci];
        if (base >= 0x1000 && base < 0x40000000 &&
            uc_mem_read(uc, base, obj, 32) == UC_ERR_OK) {
          log_info("[NULLCALL-OBS] obj@%s(0x%X): "
                   "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
                   "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
                   "%02X%02X%02X%02X %02X%02X%02X%02X",
                   cn[ci], base, obj[0], obj[1], obj[2], obj[3], obj[4],
                   obj[5], obj[6], obj[7], obj[8], obj[9], obj[10], obj[11],
                   obj[12], obj[13], obj[14], obj[15], obj[16], obj[17],
                   obj[18], obj[19], obj[20], obj[21], obj[22], obj[23],
                   obj[24], obj[25], obj[26], obj[27], obj[28], obj[29],
                   obj[30], obj[31]);
        }
      }
      /* dump LR 前 16 字节代码，便于离线看 blx 与 null 指针来源 */
      uint8_t code[16];
      if (uc_mem_read(uc, lr - 16, code, 16) == UC_ERR_OK) {
        log_info("[NULLCALL-OBS] code@LR-16: "
                 "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
                 "%02X%02X%02X%02X",
                 code[0], code[1], code[2], code[3], code[4], code[5], code[6],
                 code[7], code[8], code[9], code[10], code[11], code[12],
                 code[13], code[14], code[15]);
      }
    }
  }

  /* 返 0 是默认兜底。但若这是"服务工厂"调用（applet 经导入表取服务对象，
   * 因框架导出对象未填充而落到 blx 0），单纯的 0 会让处理器表拿到 null 句柄、
   * 游戏初始化失败而反复重初始化 → 死循环（00000434/000005f9 的表现）。
   * 机制（已逆向）：applet 调用 P->vt[id](P, id)，每个 id 对应 P 的一个不同
   * 方法、返回一种服务对象（id∈{2,3,4,7,5} 各一种），存入按 id 索引的全局表。
   * 工厂派发特征：r1 为小 id(0..0x40)、LR 落在本 applet 代码内。
   * 这里按 id 返回真实且虚表完整的框架服务对象，让处理器表拿到非空句柄以打破
   * 死循环，并使该服务对象（显示/文件/音频…）能正确工作。 */
  uint32_t ret_val = 0;
  uint32_t r1 = 0;
  uc_reg_read(uc, UC_ARM_REG_R1, &r1);
  bool is_factory = (lr >= BLOB_BASE && lr < BLOB_BASE + BLOB_SIZE && r1 <= 0x40);
  if (is_factory) {
    /* 按 ZMAEE 服务序号映射真实对象。默认（未知 id）退回 GFX 以保持兼容。
     * 该表即为 id→服务 的精确映射，后续可按 RE 结果细化。 */
    switch (r1) {
    case 0x02: ret_val = (uint32_t)GFX; break;     /* 如 IShell */
    case 0x03: ret_val = (uint32_t)GFX; break;     /* 如 IDisplay */
    case 0x04: ret_val = (uint32_t)FileMgr; break; /* 如 IFileMgr */
    case 0x05: ret_val = (uint32_t)FILE1; break;   /* 如 IFile */
    case 0x07: ret_val = (uint32_t)AUDIO; break;   /* 如 IAudio */
    default:   ret_val = (uint32_t)GFX; break;
    }
    if (s_nullcall_cnt <= 32)
      log_warn("[NULLCALL] 服务工厂 r1=0x%X → 返回服务句柄 0x%X 以打破死循环", r1,
               ret_val);
  }

  uc_reg_write(uc, UC_ARM_REG_R0, &ret_val);
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

  if (s_invalid_cnt > 128) {
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

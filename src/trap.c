#include "./trap.h"
#include "./log/log.h"
#include <inttypes.h> /* PRIx32，用于第 294 行格式化输出 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "./emu.h"
#include "./tool/odds.h"
#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/core/zm_mem.h"
#include "./zmaee/core/zm_root.h"
#include "./zmaee/core/zm_str.h"
#include "./zmaee/fs/zm_fs.h"
#include "./zmaee/gfx/zm_gfx.h"
#include "./zmaee/runtime/zm_runtime.h"
#include "event.h"
#include "zmaee/inc/zm_event_code.h"

uint32_t getArg(uc_engine *uc, uint32_t n) {
  uint64_t v64 = 0; // 关键：必须用 64 位容器
  uc_err err;

  // 1. 寄存器参数 R0 ~ R3
  if (n <= 3) {
    err = uc_reg_read(uc, UC_ARM_REG_R0 + n, &v64);
    if (err != UC_ERR_OK)
      return 0;
    return (uint32_t)v64;
  }

  // 2. 栈参数（前提：当前 PC 必须在函数第一条指令！）
  uint64_t sp = 0;
  err = uc_reg_read(uc, UC_ARM_REG_SP, &sp);
  if (err != UC_ERR_OK)
    return 0;
  if (sp == STACK_TOP) {
    log_info("sp is STACK_TOP");
    return 0;
  }
  // 第 n 个参数（n>=4）在入口栈顶的偏移 (n-4)*4
  uint64_t addr = sp + (n - 4) * 4;
  uint32_t result = 0;
  err = uc_mem_read(uc, addr, &result, 4);
  if (err != UC_ERR_OK)
    return 0;

  return result;
}
/**
 * 打印 Unicorn 模拟器中当前所有非零的 ARM32 核心寄存器
 * @param uc Unicorn 引擎句柄
 */
void print_non_zero_registers(uc_engine *uc) {
  // 定义需要遍历的寄存器结构
  typedef struct {
    int reg_id;       // Unicorn 定义的寄存器 ID (UC_ARM_REG_*)
    const char *name; // 寄存器名称（用于打印）
  } arm_reg_entry;

  // 针对 MT6250 (ARM7EJ-S) 的核心寄存器列表
  // 去掉了浮点/NEON/Cortex-M专有寄存器，因为MT6250通常不支持或不存在
  arm_reg_entry regs[] = {// 通用寄存器 R0 - R12
                          {UC_ARM_REG_R0, "R0"},
                          {UC_ARM_REG_R1, "R1"},
                          {UC_ARM_REG_R2, "R2"},
                          {UC_ARM_REG_R3, "R3"},
                          {UC_ARM_REG_R4, "R4"},
                          {UC_ARM_REG_R5, "R5"},
                          {UC_ARM_REG_R6, "R6"},
                          {UC_ARM_REG_R7, "R7"},
                          {UC_ARM_REG_R8, "R8"},
                          {UC_ARM_REG_R9, "R9"},
                          {UC_ARM_REG_R10, "R10"},
                          {UC_ARM_REG_R11, "R11"},
                          {UC_ARM_REG_R12, "R12"},
                          // 栈指针、链接寄存器、程序计数器
                          {UC_ARM_REG_SP, "SP"}, // R13
                          {UC_ARM_REG_LR, "LR"}, // R14
                          {UC_ARM_REG_PC, "PC"}, // R15
                                                 // 状态寄存器
                          {UC_ARM_REG_CPSR, "CPSR"},
                          {UC_ARM_REG_SPSR, "SPSR"}};

  int reg_count = sizeof(regs) / sizeof(regs[0]);
  uint32_t value = 0;
  uc_err err = UC_ERR_OK;
  int has_non_zero = 0; // 标记是否有非零寄存器

  printf("========== Non-zero Registers ==========\n");

  for (int i = 0; i < reg_count; i++) {
    // 读取寄存器值
    err = uc_reg_read(uc, regs[i].reg_id, &value);

    if (err == UC_ERR_OK) {
      if (value != 0) {
        printf("[+] %s: 0x%08X (%u)\n", regs[i].name, value, value);
        has_non_zero = 1;
      }
    } else {
      // 读取失败（例如在用户模式下读取 SPSR 会报错），静默跳过即可
      // 如果需要调试，可以取消下面注释：
      // printf("[!] Read %s failed (error: %d)\n", regs[i].name, err);
    }
  }

  if (!has_non_zero) {
    printf("(No non-zero registers found in current context.)\n");
  }
  printf("========================================\n");
}
void handle_trap(uc_engine *uc, uint32_t trap_address) {

  uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, sp = 0, lr = 0;
  /* 逐个检查寄存器读取结果，避免失败时使用未初始化值污染分发逻辑 */
  if (uc_reg_read(uc, UC_ARM_REG_R0, &r0) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_R1, &r1) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_R2, &r2) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_R3, &r3) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_SP, &sp) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_LR, &lr) != UC_ERR_OK) {
    log_error("Failed to read core registers at trap 0x%08" PRIx32,
              trap_address);
    return;
  }
  // 随便打印十个参数试一下,不一定是参数啊但是说实话应该有参数超过四个的情况所以这里打印十个看一下
  for (int i = 0; i < 10; i++) {
    uint32_t v = getArg(uc, i);
    printf("arg%d: 0x%08X\n", i, v);
  }
  // log_debug("trap addr: %d, r0: %d, r1: %d, r2: %d, r3: %d, sp: %d, lr:
  // %d\n",
  //           trap_address, r0, r1, r2, r3, sp, lr);

  print_non_zero_registers(uc);

  uint32_t ret = 0;
  switch (trap_address) {
  case TR_init_callback: { //  /* TR_init_callback：特殊处理（不写 R0/PC
                           //  走通用路径，而是直接跳 handler） */

    uint32_t size; // 从这个槽里面读出它要申请的堆大小
    if (uc_mem_read(uc, SIZE_SLOT, &size, 4) != UC_ERR_OK) {
      log_error("Failed to read size");
      return;
    }
    uint32_t handler;
    if (uc_mem_read(uc, API_SLOT + 8, &handler, 4) != UC_ERR_OK) {
      log_error("Failed to read handler");
      return;
    }

    log_info("  size=%d handler=0x%X\n", size, handler);
    uint32_t INSTANCE = host_malloc(&g_heap_ptr, size);

    uint8_t *zero_buf = calloc(1, size);
    uc_mem_write(uc, INSTANCE, zero_buf, size);
    free(zero_buf);

    /* 把当前 applet 短名称写入 instance+4；applet 用它在运行时构造
     * "<name>.zmr" 等资源文件名。使用短名而非完整路径，避免污染
     * instance 边界外的堆内存。 */
    const char *filename = get_filename_from_fullpath(g_app_pathname);
    size_t fn_len = strlen(filename) + 1; /* 含结尾 '\0' */
    /* 边界校验：仅当 (INSTANCE+4+fn_len) 落在 [INSTANCE, INSTANCE+size]
     * 范围内才写入，防止越界覆盖 instance 之外的堆内存。 */
    if (4 + fn_len <= (size_t)size) {
      uc_mem_write(uc, INSTANCE + 4, filename, fn_len);
    } else {
      log_warn(
          "filename (len=%zu) too long for instance(%u), skip writing name",
          fn_len, size);
    }
    log_info("filename: %s\n", filename);
    log_info("  instance=0x%X\n", INSTANCE);

    uc_reg_write(uc, UC_ARM_REG_R0, &INSTANCE);
    // 参数一是事件类型码
    uint32_t event_code = ZMAEE_EV_CREATE;
    uc_reg_write(uc, UC_ARM_REG_R1, &event_code);
    // 零表示常规启动
    uint32_t init_type = 0;
    uc_reg_write(uc, UC_ARM_REG_R2, &init_type); //
    /* 00000405.app：init wrapper sub_8433C 在 a2==0 时解引用
     * a3[64]（r3+0x100）。 传入 INIT_CTX（256B 零填充）使其可读且
     * *a3=0≠1、a3[64]=0≠4 → init 继续。 */
    uint32_t init_ctx = INIT_CTX;
    uc_reg_write(uc, UC_ARM_REG_R3, &init_ctx);

    uint32_t callback_addr = TR_enter_event_loop;
    uc_reg_write(uc, UC_ARM_REG_LR, &callback_addr);

    g_instance = INSTANCE;
    g_handler = handler;

    uc_reg_write(uc, UC_ARM_REG_PC, &handler);
    return;
  } break;
  case TR_enter_event_loop: {
    /* 事件循环：阻塞直到用户关窗（SDL_QUIT）或超时（ZM_GFX_HOLD_MS）。
     * 返回 false → 模拟应结束；返回 true → 已派发点击，模拟器继续执行
     * handler。必须 uc_emu_stop + return，否则会 fall-through 到 default
     * 误报"非法的外部调用"，且 PC 继续执行 TRAMP 区下一条指令导致越界。 */
    uint32_t hold_ms = 0;
    const char *env = getenv("ZM_GFX_HOLD_MS");
    if (env && *env)
      hold_ms = (uint32_t)strtoul(env, NULL, 0);
    if (!zm_gfx_event_loop(on_touch_click, hold_ms)) {
      uc_emu_stop(uc);
    }
    return;
  } //
  break;
    /* ---- ROOT ---- */
  case TR_root_getShell:
    ret = RUNTIME;
    break;
  case TR_root_malloc:
    ret = host_malloc(&g_heap_ptr, r0);
    break; /* malloc */
  case TR_root_free:
    log_debug("这里的话是 free(r0=%d)", r0);
    ret = 0;
    break; /* free */
  case TR_root_str_copy:
    ret = zm_strcpy(uc, r0, r1, r2, r3);
    break; /* str_copy */
  case TR_root_sprintf:
    ret = zm_sprintf(uc, r0, r1, r2);
    break; /* sprintf */
  case TR_root_str_ctor:
    ret = zm_strcpy_cstr(uc, r0, r1);
    break; /* str_ctor -> strcpy_cstr */
  case TR_root_spec_lookup:
    ret = zm_spec_lookup(uc, r0);
    break; /* spec_lookup */
  case TR_root_str_find:
    ret = zm_strchr(uc, r0, r1);
    break; /* str_find -> strchr */
  /* ---- runtime ---- */
  case TR_rt_queryInterface:
    ret = zm_rt_queryInterface(uc, r1, r2);
    break;
  case TR_rt_getSystemInfo:
    ret = zm_rt_getSystemInfo(uc, r1);
    break;
  /* ---- gfx ---- */
  case TR_gfx_clear:
    ret = zm_gfx_clear(uc, r1);
    break;
  case TR_gfx_fillRect:
    ret = zm_gfx_fillRect(uc, r1);
    break;
  case TR_gfx_commit:
    ret = zm_gfx_commit(uc);
    break;
  case TR_gfx_drawText:
    ret = zm_gfx_drawText(uc, r1, r2, r3, sp);
    break;
  case TR_gfx_drawRect:
    ret = zm_gfx_drawRect(uc, r1, r2, r3, sp);
    break;
  case TR_gfx_fillRect2:
    ret = zm_gfx_fillRect2(uc, r1, r2, r3, sp);
    break;
  /* ---- fs / file ---- */
  case TR_fileMgr_open_file:
    log_debug("fs_open(r0=0x%X)", r0); // 此处的 r0 是 FileMgr的地址
    log_debug("FileMgr=0x%X", FileMgr);

    if (r0 == 0) {
      ret = 0;
    } else {
      ret = zm_fileMgr_open_file(uc, r1); // 这地方为什么是r1呀
    }
    break;
  case TR_file_close:
    ret = zm_file_close(uc, r0);
    break; /* file.close(r0=this/file_id) */
  case TR_file_read:
    ret = zm_file_read(uc, r0, r1, r2);
    break;
  case TR_file_seek:
    ret = zm_file_seek(uc, r0, r1, r2);
    break;
  case TR_file_size:
    ret = zm_file_size(uc, r0);
    break; /* FILE_VT[0x24] file.size */
  /* ---- audio / ap ---- */
  case TR_audio_stop:
    ret = zm_audio_stop(uc);
    break;
  case TR_ap_play:
    ret = zm_ap_play(uc, r2, r3);
    break;
  case TR_ap_stop:
    ret = zm_ap_stop(uc);
    break;
  case TR_audio_get_status:
    ret = zm_audio_get_status(uc, r1, r2);
    break; /* AUDIO_VT[0x24] */
    /* ---- 00000405.app：GFX vtable 缺失槽 ---- */

  /* ---- 服务对象 / FS / RT / DLL / CBK ---- */
  case TR_svc_release:
    ret = zm_svc_release(uc);
    break;
  case TR_svc04_x1C:
    ret = zm_svc04_x1C(uc, r0, r1, r2, r3);
    break;
  case TR_svc09_x2C:
    ret = zm_svc09_x2C(uc, r0, r1, r2, r3);
    break;
  case TR_svc09_x40:
    ret = zm_svc09_x40(uc, r0, r1, r2, r3);
    break;
  case TR_fs_release:
    ret = zm_fs_release(uc);
    break;
  case TR_rt_loadDLL:
    ret = zm_rt_loadDLL(uc, r1, r2, r3);
    break;
  case TR_rt_unloadDLL:
    ret = zm_rt_unloadDLL(uc, r1);
    break;
  case TR_rt_loadDLL2:
    ret = zm_rt_loadDLL2(uc, r0, r1, r2, r3);
    break; /* RT_VT[0x78] */
  case TR_dll_init:
    ret = zm_dll_init(uc);
    break;
  case TR_dll_config:
    ret = zm_dll_config(uc, r1, r2, r3);
    break;
  case TR_dll_entry:
    ret = zm_dll_entry(uc, r1, r2, r3);
    break;
  case TR_cbk_default:
    ret = zm_root_cbk_default(uc, r0, r1, r2, r3);
    break;
  default:
    log_error("非法的外部调用: 0x%08" PRIx32, trap_address);
    pause_console();
    break;
  }

  log_debug("applet 调用外部的返回值是 r0等于0x%X", ret);

  uc_reg_write(uc, UC_ARM_REG_R0, &ret);

  uc_reg_write(uc, UC_ARM_REG_PC, &lr);
}

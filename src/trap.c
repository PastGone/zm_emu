#include "./trap.h"
#include "./log/log.h"
#include <inttypes.h>
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

void handle_trap(uc_engine *uc, uint32_t trap_address) {

  uint32_t r0, r1, r2, r3, sp, lr;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  uc_reg_read(uc, UC_ARM_REG_R1, &r1);
  uc_reg_read(uc, UC_ARM_REG_R2, &r2);
  uc_reg_read(uc, UC_ARM_REG_R3, &r3);
  uc_reg_read(uc, UC_ARM_REG_SP, &sp);
  uc_reg_read(uc, UC_ARM_REG_LR, &lr);

  log_debug("trap addr: %d, r0: %d, r1: %d, r2: %d, r3: %d, sp: %d, lr: %d\n",
            trap_address, r0, r1, r2, r3, sp, lr);

  uint32_t ret = 0;
  switch (trap_address) {
  case TR_init_callback: { //  /* TR_init_callback：特殊处理（不写 R0/PC
                           //  走通用路径，而是直接跳 handler） */

    uint32_t size;
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
    uc_mem_write(uc, INSTANCE + 4, filename, strlen(filename) + 1);
    log_info("filename: %s\n", filename);
    log_info("  instance=0x%X\n", INSTANCE);

    uc_reg_write(uc, UC_ARM_REG_R0, &INSTANCE);
    uint32_t zero = 0;
    uc_reg_write(uc, UC_ARM_REG_R1, &zero);
    uc_reg_write(uc, UC_ARM_REG_R2, &zero);
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
     * 循环返回 = 模拟应结束。必须 uc_emu_stop + return，否则会
     * fall-through 到下方 switch：idx = (TR_event_callback - TRAMP_BASE)/4
     * = 465（0721 八进制 = 465 十进制）命中 default 误报"非法的外部调用
     * idx=465"，且 PC=LR=TR_event_callback 反复重入导致 0x2001b8 越界。 */
    /* ZM_GFX_HOLD_MS：仅用于无头/自动化测试时让事件循环超时返回（与
     * test_diag 一致）。默认 0 = 直到关窗，交互行为不变。 */
    uint32_t hold_ms = 0;
    const char *env = getenv("ZM_GFX_HOLD_MS");
    if (env && *env)
      hold_ms = (uint32_t)strtoul(env, NULL, 0);
    zm_gfx_event_loop(on_touch_click, hold_ms);
    uc_emu_stop(uc);
    return;
  } //
  break;
    /* ---- ROOT ---- */
  case TR_root_queryRuntime:
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

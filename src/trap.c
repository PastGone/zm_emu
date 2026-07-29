#include "./trap.h"
#include "./log/log.h"
#include <inttypes.h>
#include <string.h>

#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/core/zm_addrs.h"
#include "./zmaee/core/zm_mem.h"
#include "./zmaee/core/zm_str.h"
#include "./zmaee/fs/zm_fs.h"
#include "./zmaee/gfx/zm_gfx.h"
#include "./zmaee/runtime/zm_runtime.h"

void handle_trap(uc_engine *uc, uint32_t trap_address, uint32_t r0, uint32_t r1,
                 uint32_t r2, uint32_t r3, uint32_t sp, uint32_t lr) {
  uint32_t ret = 0;

  if (trap_address == TR_init_callback) {
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
    uint32_t INSTANCE = host_malloc(&heap_ptr, size);
    uint8_t *zero_buf = calloc(1, size);
    uc_mem_write(uc, INSTANCE, zero_buf, size);
    free(zero_buf);

    char app_name[64];
    snprintf(app_name, sizeof(app_name), "%.*s.app",
             (int)sizeof(header.AppName), header.AppName);
    uc_mem_write(uc, INSTANCE + 4, app_name, strlen(app_name) + 1);
    log_info("AppName: %s\n", app_name);
    log_info("  instance=0x%X\n", INSTANCE);

    uint32_t stack_ptr = STACK_TOP;
    uc_reg_write(uc, UC_ARM_REG_SP, &stack_ptr);

    uc_reg_write(uc, UC_ARM_REG_R0, &INSTANCE);
    uint32_t zero = 0;
    uc_reg_write(uc, UC_ARM_REG_R1, &zero);
    uc_reg_write(uc, UC_ARM_REG_R2, &zero);
    uc_reg_write(uc, UC_ARM_REG_R3, &zero);
    uint32_t end_addr = STACK_TOP;
    uc_reg_write(uc, UC_ARM_REG_LR, &end_addr);

    g_instance = INSTANCE;
    g_handler = handler;

    uc_reg_write(uc, UC_ARM_REG_PC, &handler);
    return;
  }
  // root
  else if (trap_address == TR_root_queryRuntime) {
    ret = RUNTIME;
  } else if (trap_address == TR_root_malloc) {
    ret = host_malloc(&heap_ptr, r0);
  } else if (trap_address == TR_root_free) {
    ret = 0;
  } else if (trap_address == TR_root_str_copy) {
    ret = zm_strcpy(uc, r0, r1, r2, r3);
  } else if (trap_address == TR_root_sprintf) {
    ret = zm_sprintf(uc, r0, r1, r2);
  } else if (trap_address == TR_root_str_ctor) {
    ret = zm_str_ctor(uc, r0, r1);
  } else if (trap_address == TR_root_spec_lookup) {
    ret = zm_spec_lookup(uc, r0);
  } else if (trap_address == TR_root_str_find) {
    ret = zm_str_find(uc, r0, r1);
  }
  /* ---- runtime ---- */
  else if (trap_address == TR_rt_queryInterface) {
    ret = zm_rt_queryInterface(uc, r1, r2);
  } else if (trap_address == TR_rt_getSystemInfo) {
    ret = zm_rt_getSystemInfo(uc, r1);
  }
  /* ---- gfx ---- */
  else if (trap_address == TR_gfx_clear) {
    ret = zm_gfx_clear(uc, r1);
  } else if (trap_address == TR_gfx_fillRect) {
    ret = zm_gfx_fillRect(uc, r1);
  } else if (trap_address == TR_gfx_commit) {
    ret = zm_gfx_commit(uc);
  } else if (trap_address == TR_gfx_drawText) {
    ret = zm_gfx_drawText(uc, r1, r2, r3, sp);
  } else if (trap_address == TR_gfx_drawRect) {
    ret = zm_gfx_drawRect(uc, r1, r2, r3, sp);
  } else if (trap_address == TR_gfx_fillRect2) {
    ret = zm_gfx_fillRect2(uc, r1, r2, r3, sp);
  }
  /* ---- fs / file ---- */
  else if (trap_address == TR_fs_open) {
    ret = zm_fs_open(uc, r1);
  } else if (trap_address == TR_file_close) {
    ret = zm_file_close(uc);
  } else if (trap_address == TR_file_read) {
    ret = zm_file_read(uc, r1, r2);
  } else if (trap_address == TR_file_seek) {
    ret = zm_file_seek(uc, r1, r2);
  }
  /* ---- audio / ap ---- */
  else if (trap_address == TR_audio_stop) {
    ret = zm_audio_stop(uc);
  } else if (trap_address == TR_ap_play) {
    ret = zm_ap_play(uc, r2, r3);
  } else if (trap_address == TR_ap_stop) {
    ret = zm_ap_stop(uc);
  } else {
    log_error("非法的外部调用: 0x%08" PRIx32, trap_address);
    log_error("其陷阱号是: %d", (trap_address - TRAMP_BASE) / 4);
  }

  uc_reg_write(uc, UC_ARM_REG_R0, &ret);
  uc_reg_write(uc, UC_ARM_REG_PC, &lr);
}
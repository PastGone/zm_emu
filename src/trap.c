#include "./trap.h"
#include "./log/log.h"
#include <inttypes.h>
#include <string.h>

#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/core/zm_addrs.h"
#include "./zmaee/core/zm_mem.h"
#include "./zmaee/core/zm_root.h"
#include "./zmaee/core/zm_str.h"
#include "./zmaee/fs/zm_fs.h"
#include "./zmaee/gfx/zm_gfx.h"
#include "./zmaee/runtime/zm_runtime.h"

void handle_trap(uc_engine *uc, uint32_t trap_address, uint32_t r0, uint32_t r1,
                 uint32_t r2, uint32_t r3, uint32_t sp, uint32_t lr) {
  uint32_t ret = 0;

  /* TR_init_callback：特殊处理（不写 R0/PC 走通用路径，而是直接跳 handler） */
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

    char *filename =
        "00000102.app"; // 这个地方传的是它在文件管理器面显示的名称当然也有可能是路径反正不是他解码之后文件头里面的那个名称
    uc_mem_write(uc, INSTANCE + 4, filename, strlen(filename) + 1);
    log_info("filename: %s\n", filename);
    log_info("  instance=0x%X\n", INSTANCE);

    uint32_t stack_ptr = STACK_TOP;
    uc_reg_write(uc, UC_ARM_REG_SP, &stack_ptr);

    uc_reg_write(uc, UC_ARM_REG_R0, &INSTANCE);
    uint32_t zero = 0;
    uc_reg_write(uc, UC_ARM_REG_R1, &zero);
    uc_reg_write(uc, UC_ARM_REG_R2, &zero);
    /* 00000405.app：init wrapper sub_8433C 在 a2==0 时解引用
     * a3[64]（r3+0x100）。 传入 INIT_CTX（256B 零填充）使其可读且
     * *a3=0≠1、a3[64]=0≠4 → init 继续。 */
    uint32_t init_ctx = INIT_CTX;
    uc_reg_write(uc, UC_ARM_REG_R3, &init_ctx);
    uint32_t end_addr = STACK_TOP;
    uc_reg_write(uc, UC_ARM_REG_LR, &end_addr);

    g_instance = INSTANCE;
    g_handler = handler;

    uc_reg_write(uc, UC_ARM_REG_PC, &handler);
    return;
  }

  /* 其余 trap 按 (trap_address - TRAMP_BASE)/4 索引分发 */
  int idx = (int)((trap_address - TRAMP_BASE) / 4);
  switch (idx) {
  /* ---- ROOT ---- */
  case 0:
    ret = RUNTIME;
    break; /* queryRuntime */
  case 1:
    ret = host_malloc(&heap_ptr, r0);
    break; /* malloc */
  case 2:
    log_debug("这里的话是 free(r0=%d)", r0);
    ret = 0;
    break; /* free */
  case 3:
    ret = zm_strcpy(uc, r0, r1, r2, r3);
    break; /* str_copy */
  case 4:
    ret = zm_sprintf(uc, r0, r1, r2);
    break; /* sprintf */
  case 5:
    ret = zm_str_ctor(uc, r0, r1);
    break; /* str_ctor */
  case 6:
    ret = zm_spec_lookup(uc, r0);
    break; /* spec_lookup */
  case 7:
    ret = zm_str_find(uc, r0, r1);
    break; /* str_find */
  /* ---- runtime ---- */
  case 8:
    ret = zm_rt_queryInterface(uc, r1, r2);
    break;
  case 9:
    ret = zm_rt_getSystemInfo(uc, r1);
    break;
  /* ---- gfx ---- */
  case 10:
    ret = zm_gfx_clear(uc, r1);
    break;
  case 11:
    ret = zm_gfx_fillRect(uc, r1);
    break;
  case 12:
    ret = zm_gfx_commit(uc);
    break;
  case 13:
    ret = zm_gfx_drawText(uc, r1, r2, r3, sp);
    break;
  case 14:
    ret = zm_gfx_drawRect(uc, r1, r2, r3, sp);
    break;
  case 15:
    ret = zm_gfx_fillRect2(uc, r1, r2, r3, sp);
    break;
  /* ---- fs / file ---- */
  case 16:
    ret = zm_fs_open(uc, r1);
    break;
  case 17:
    ret = zm_file_close(uc, r0);
    break; /* file.close(r0=this/file_id) */
  case 18:
    ret = zm_file_read(uc, r0, r1, r2);
    break;
  case 19:
    ret = zm_file_seek(uc, r0, r1, r2);
    break;
  case 58:
    ret = zm_file_size(uc, r0);
    break; /* FILE_VT[0x24] file.size */
  /* ---- audio / ap ---- */
  case 20:
    ret = zm_audio_stop(uc);
    break;
  case 21:
    ret = zm_ap_play(uc, r2, r3);
    break;
  case 22:
    ret = zm_ap_stop(uc);
    break;
  case 60:
    ret = zm_audio_get_status(uc, r1, r2);
    break; /* AUDIO_VT[0x24] */
  /* ---- 00000405.app：GFX vtable 缺失槽 ---- */
  case 61:
    ret = zm_gfx_stub(uc, 0x18, r0, r1, r2, r3);
    break; /* GFX_VT[0x18] */
  case 62:
    ret = zm_gfx_stub(uc, 0x34, r0, r1, r2, r3);
    break; /* GFX_VT[0x34] */
  case 63:
    ret = zm_gfx_stub(uc, 0x38, r0, r1, r2, r3);
    break; /* GFX_VT[0x38] */
  case 64:
    ret = zm_gfx_stub(uc, 0x44, r0, r1, r2, r3);
    break; /* GFX_VT[0x44] */
  case 65:
    ret = zm_gfx_get_width(uc);
    break; /* GFX_VT[0x48] */
  case 66:
    ret = zm_gfx_stub(uc, 0x54, r0, r1, r2, r3);
    break; /* GFX_VT[0x54] */
  case 67:
    ret = zm_gfx_stub(uc, 0x68, r0, r1, r2, r3);
    break; /* GFX_VT[0x68] */
  case 68:
    ret = zm_gfx_stub(uc, 0x94, r0, r1, r2, r3);
    break; /* GFX_VT[0x94] */
  case 69:
    /* GFX_VT[0xA4]：createImage(gfx, buf, malloc, free, &out_obj)
     * sub_8081C 用它创建背景位图对象；返回 0（成功）但不写 out_obj 会导致
     * applet 解引用 null。返回 1（失败）使 applet 跳过位图绘制块，
     * 直接走文本绘制路径（gfx[0x50] drawText）。 */
    log_info("gfx[0xA4] createImage -> 1 (fail, skip tile draw)");
    ret = 1;
    break; /* GFX_VT[0xA4] */
  case 70:
    ret = zm_gfx_stub(uc, 0xB0, r0, r1, r2, r3);
    break; /* GFX_VT[0xB0] */
  case 71:
    ret = zm_gfx_stub(uc, 0x0C, r0, r1, r2, r3);
    break; /* GFX_VT[0x0C]：setClipRect */
  case 72:
    ret = zm_gfx_stub(uc, 0x28, r0, r1, r2, r3);
    break; /* GFX_VT[0x28]：flush */
  case 73:
    ret = zm_gfx_measure_char(uc, r0, r1, r2, r3);
    break; /* GFX_VT[0x4C]：measureChar */
  case 74:
    ret = zm_fs_enum(uc, r0, r1);
    break; /* FS_VT[0x30]：enumFile */
  /* ---- 00000405.app：ROOT vtable 缺失槽 ---- */
  case 23:
    ret = zm_root_stub(uc, 0x18, r0, r1, r2, r3);
    break;
  case 24:
    ret = zm_root_stub(uc, 0x1C, r0, r1, r2, r3);
    break;
  case 25:
    ret = zm_root_stub(uc, 0x24, r0, r1, r2, r3);
    break;
  case 26:
    ret = zm_root_stub(uc, 0x50, r0, r1, r2, r3);
    break;
  case 27:
    ret = zm_root_stub(uc, 0x5C, r0, r1, r2, r3);
    break;
  case 28:
    ret = zm_root_memset(uc, r0, r1, r2);
    break; /* ROOT[0x60] */
  case 29:
    ret = zm_root_stub(uc, 0x74, r0, r1, r2, r3);
    break;
  case 30:
    ret = zm_root_str_assign(uc, r0, r1);
    break; /* ROOT[0x78] */
  case 31:
    ret = zm_root_stub(uc, 0x7C, r0, r1, r2, r3);
    break;
  case 32:
    ret = zm_root_stub(uc, 0x80, r0, r1, r2, r3);
    break;
  case 33:
    ret = zm_root_stub(uc, 0x84, r0, r1, r2, r3);
    break;
  case 34:
    ret = zm_root_stub(uc, 0x8C, r0, r1, r2, r3);
    break;
  case 35:
    ret = zm_root_stub(uc, 0x90, r0, r1, r2, r3);
    break;
  case 36:
    ret = zm_root_stub(uc, 0xB0, r0, r1, r2, r3);
    break;
  case 37:
    ret = zm_root_stub(uc, 0xC0, r0, r1, r2, r3);
    break;
  case 38:
    ret = zm_root_stub(uc, 0xC8, r0, r1, r2, r3);
    break;
  case 39:
    ret = zm_root_stub(uc, 0xD0, r0, r1, r2, r3);
    break;
  case 40:
    ret = zm_root_get_tick(uc);
    break; /* ROOT[0xD8] */
  case 41:
    ret = zm_root_stub(uc, 0x12C, r0, r1, r2, r3);
    break;
  case 42:
    ret = zm_root_stub(uc, 0x130, r0, r1, r2, r3);
    break;
  case 43:
    ret = zm_root_stub(uc, 0x140, r0, r1, r2, r3);
    break;
  case 44:
    ret = zm_root_create_cbk(uc);
    break; /* ROOT[0x154] */
  case 45:
    ret = zm_root_stub(uc, 0x16C, r0, r1, r2, r3);
    break;
  /* ---- 服务对象 / FS / RT / DLL / CBK ---- */
  case 46:
    ret = zm_svc_release(uc);
    break;
  case 47:
    ret = zm_svc04_x1C(uc, r0, r1, r2, r3);
    break;
  case 48:
    ret = zm_svc09_x2C(uc, r0, r1, r2, r3);
    break;
  case 49:
    ret = zm_svc09_x40(uc, r0, r1, r2, r3);
    break;
  case 50:
    ret = zm_fs_release(uc);
    break;
  case 51:
    ret = zm_fs_chdir(uc, r1);
    break;
  case 52:
    ret = zm_rt_loadDLL(uc, r1, r2, r3);
    break;
  case 53:
    ret = zm_rt_unloadDLL(uc, r1);
    break;
  case 59:
    ret = zm_rt_loadDLL2(uc, r0, r1, r2, r3);
    break; /* RT_VT[0x78] */
  case 54:
    ret = zm_dll_init(uc);
    break;
  case 55:
    ret = zm_dll_config(uc, r1, r2, r3);
    break;
  case 56:
    ret = zm_dll_entry(uc, r1, r2, r3);
    break;
  case 57:
    ret = zm_root_cbk_default(uc, r0, r1, r2, r3);
    break;
  default:
    log_error("非法的外部调用: 0x%08" PRIx32 " (idx=%d)", trap_address, idx);
    break;
  }

  uc_reg_write(uc, UC_ARM_REG_R0, &ret);
  uc_reg_write(uc, UC_ARM_REG_PC, &lr);
}

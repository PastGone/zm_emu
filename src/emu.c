#include "./emu.h"
#include "./hook.h"
#include "./log/log.h"
#include "./zmaee/fs/zm_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------- 全局变量定义 -------------------- */
uc_engine *uc;
AppHeader header;
uint32_t heap_ptr = HEAP_BASE;

uint32_t g_instance = 0;
uint32_t g_handler = 0;
int g_trap_pause = 0;
int g_disasm = 0;

csh handle;
cs_insn *insn;
size_t count;
uint8_t code[16];

/* -------------------- shim 虚表地址定义 -------------------- */
uint32_t ROOT = SHIM_BASE + 0x000;
uint32_t RUNTIME = SHIM_BASE + 0x100;
uint32_t RT_VT = SHIM_BASE + 0x180;
uint32_t GFX = SHIM_BASE + 0x200;
uint32_t GFX_VT = SHIM_BASE + 0x280;
uint32_t FS = SHIM_BASE + 0x300;
uint32_t FS_VT = SHIM_BASE + 0x380;
uint32_t FILE1 = SHIM_BASE + 0x400;
uint32_t FILE_VT = SHIM_BASE + 0x480;
uint32_t AUDIO = SHIM_BASE + 0x500;
uint32_t AUDIO_VT = SHIM_BASE + 0x580;
uint32_t AP = SHIM_BASE + 0x600;
uint32_t AP_VT = SHIM_BASE + 0x680;
uint32_t DUMMY_BUF = SHIM_BASE + 0x750;

/* -------------------- trap 地址定义 -------------------- */
uint32_t TR_root_queryRuntime = TRAP(0);
uint32_t TR_root_malloc = TRAP(1);
uint32_t TR_root_free = TRAP(2);
uint32_t TR_root_str_copy = TRAP(3);
uint32_t TR_root_sprintf = TRAP(4);
uint32_t TR_root_str_ctor = TRAP(5);
uint32_t TR_root_spec_lookup = TRAP(6);
uint32_t TR_root_str_find = TRAP(7);

uint32_t TR_rt_queryInterface = TRAP(8);
uint32_t TR_rt_getSystemInfo = TRAP(9);

uint32_t TR_gfx_clear = TRAP(10);
uint32_t TR_gfx_fillRect = TRAP(11);
uint32_t TR_gfx_commit = TRAP(12);
uint32_t TR_gfx_drawText = TRAP(13);
uint32_t TR_gfx_drawRect = TRAP(14);
uint32_t TR_gfx_fillRect2 = TRAP(15);

uint32_t TR_fs_open = TRAP(16);
uint32_t TR_file_close = TRAP(17);
uint32_t TR_file_read = TRAP(18);
uint32_t TR_file_seek = TRAP(19);

uint32_t TR_audio_stop = TRAP(20);
uint32_t TR_ap_play = TRAP(21);
uint32_t TR_ap_stop = TRAP(22);

uint32_t TR_init_callback = TRAP(100);
uint32_t SIZE_SLOT = SHIM_BASE + 0x700;
uint32_t API_SLOT = SHIM_BASE + 0x710;

/* -------------------- 实现 -------------------- */

int zm_emu_map_memory(uc_engine *uc) {
  uc_err err;
  err = uc_mem_map(uc, BLOB_BASE, BLOB_SIZE, UC_PROT_ALL);
  err = uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  err = uc_mem_map(uc, HEAP_BASE, HEAP_SIZE, UC_PROT_ALL);
  err = uc_mem_map(uc, SHIM_BASE, SHIM_SIZE, UC_PROT_ALL);
  err = uc_mem_map(uc, TRAMP_BASE, TRAMP_SIZE, UC_PROT_ALL);
  err = uc_mem_map(uc, ZMR_BASE, ZMR_SIZE, UC_PROT_ALL);
  if (err != UC_ERR_OK) {
    log_error("uc_mem_map failed, err: %d\n", err);
    return -1;
  }
  log_info("内存映射完成");
  return 0;
}

int zm_emu_build_vtables(uc_engine *uc) {
  uc_err err;
  // root
  err = uc_mem_write(uc, ROOT, &TR_root_queryRuntime, 4);
  err = uc_mem_write(uc, ROOT + 0x008, &TR_root_malloc, 4);
  err = uc_mem_write(uc, ROOT + 0x00C, &TR_root_free, 4);
  err = uc_mem_write(uc, ROOT + 0x020, &TR_root_str_copy, 4);
  err = uc_mem_write(uc, ROOT + 0x06C, &TR_root_sprintf, 4);
  err = uc_mem_write(uc, ROOT + 0x088, &TR_root_str_ctor, 4);
  err = uc_mem_write(uc, ROOT + 0x0A4, &TR_root_spec_lookup, 4);
  err = uc_mem_write(uc, ROOT + 0x0A8, &TR_root_str_find, 4);

  // runtime
  err = uc_mem_write(uc, RUNTIME, &RT_VT, 4);
  err = uc_mem_write(uc, RT_VT + 0x08, &TR_rt_queryInterface, 4);
  err = uc_mem_write(uc, RT_VT + 0x10, &TR_rt_getSystemInfo, 4);
  // gfx
  err = uc_mem_write(uc, GFX, &GFX_VT, 4);
  err = uc_mem_write(uc, GFX_VT + 0x20, &TR_gfx_clear, 4);
  err = uc_mem_write(uc, GFX_VT + 0x2C, &TR_gfx_fillRect, 4);
  err = uc_mem_write(uc, GFX_VT + 0x40, &TR_gfx_commit, 4);
  err = uc_mem_write(uc, GFX_VT + 0x50, &TR_gfx_drawText, 4);
  err = uc_mem_write(uc, GFX_VT + 0x6C, &TR_gfx_drawRect, 4);
  err = uc_mem_write(uc, GFX_VT + 0x70, &TR_gfx_fillRect2, 4);

  // fs
  err = uc_mem_write(uc, FS, &FS_VT, 4);
  err = uc_mem_write(uc, FS_VT + 0x08, &TR_fs_open, 4);
  err = uc_mem_write(uc, FILE1, &FILE_VT, 4);
  err = uc_mem_write(uc, FILE_VT + 0x04, &TR_file_close, 4);
  err = uc_mem_write(uc, FILE_VT + 0x08, &TR_file_read, 4);
  err = uc_mem_write(uc, FILE_VT + 0x20, &TR_file_seek, 4);
  // audio
  err = uc_mem_write(uc, AUDIO, &AUDIO_VT, 4);
  err = uc_mem_write(uc, AUDIO_VT + 0x14, &TR_audio_stop, 4);
  err = uc_mem_write(uc, AP, &AP_VT, 4);
  err = uc_mem_write(uc, AP_VT + 0x10, &TR_ap_play, 4);
  err = uc_mem_write(uc, AP_VT + 0x14, &TR_ap_stop, 4);
  if (err != UC_ERR_OK) {
    log_error("uc_mem_write failed, err: %d\n", err);
    return -1;
  }
  log_info("虚表构建完成");
  return 0;
}

int zm_emu_load_blob(uc_engine *uc, const char *filename, long *out_size) {
  FILE *fp = fopen(filename, "rb");
  if (fp == NULL) {
    log_error("fopen failed");
    return -1;
  }

  fseek(fp, 0, SEEK_END);
  long applet_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  log_info("文件大小: %ld\n", applet_size);
  *out_size = applet_size;

  log_info("开始载入blob数据");
  unsigned char *buf = malloc(applet_size);
  if (buf == NULL) {
    log_error("malloc failed");
    fclose(fp);
    return -1;
  }

  size_t bytes_read = fread(buf, 1, applet_size, fp);
  if (bytes_read != (size_t)applet_size) {
    log_error("读取文件失败，期望%zu字节，实际读取%zu", (size_t)applet_size,
              bytes_read);
    free(buf);
    fclose(fp);
    return -1;
  }

  uc_err err = uc_mem_write(uc, BLOB_BASE, buf, applet_size);
  if (err != UC_ERR_OK) {
    log_error("uc_mem_write failed, err: %d\n", err);
    free(buf);
    fclose(fp);
    return -1;
  }
  free(buf);
  fclose(fp);
  log_info("blob数据载入完成");
  log_info("文件关闭完成");
  return 0;
}

void zm_emu_load_zmr_if_exists(uc_engine *uc, const char *app_path) {
  char zmr_path[1024];
  strncpy(zmr_path, app_path, sizeof(zmr_path) - 1);
  zmr_path[sizeof(zmr_path) - 1] = '\0';
  size_t plen = strlen(zmr_path);
  if (plen >= 4 && strcmp(zmr_path + plen - 4, ".app") == 0) {
    strcpy(zmr_path + plen - 4, ".zmr");
  } else {
    strncat(zmr_path, ".zmr", sizeof(zmr_path) - plen - 1);
  }

  FILE *zmr_fp = fopen(zmr_path, "rb");
  if (zmr_fp) {
    fclose(zmr_fp);
    if (!zm_fs_load_zmr(zmr_path)) {
      log_warn(".zmr 载入失败，跳过: %s", zmr_path);
    } else {
      log_info(".zmr 资源载入完成: %s", zmr_path);
    }
  } else {
    log_info("未找到 .zmr 文件，跳过资源载入: %s", zmr_path);
  }
}

int zm_emu_add_hooks(uc_engine *uc, uc_hook *hook_code_h,
                     uc_hook *hook_unmapped_h, uc_hook *hook_shim_h) {
  uc_err err;
  log_info("添加钩子");

  err =
      uc_hook_add(uc, hook_code_h, UC_HOOK_CODE, (void *)hook_code, NULL, 1, 0);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add failed, err: %d\n", err);
    return -1;
  }

  err = uc_hook_add(uc, hook_unmapped_h, UC_HOOK_MEM_UNMAPPED,
                    (void *)hook_mem_unmapped, NULL, 1, 0);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add failed, err: %d\n", err);
    return -1;
  }

  err = uc_hook_add(uc, hook_shim_h, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
                    (void *)hook_shim_mem, NULL, SHIM_BASE,
                    SHIM_BASE + SHIM_SIZE);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add failed, err: %d\n", err);
    return -1;
  }
  log_info("钩子添加完成");
  return 0;
}

int zm_emu_start_applet(uc_engine *uc) {
  uc_reg_write(uc, UC_ARM_REG_LR, &TR_init_callback);
  uc_reg_write(uc, UC_ARM_REG_R0, &SIZE_SLOT);
  uc_reg_write(uc, UC_ARM_REG_R1, &API_SLOT);

  uc_mem_write(uc, BLOB_BASE + ROOT_SLOT_OFF, &ROOT, 4);

  log_info("启动unicorn engine...");
  uc_emu_start(uc, APPLET_ENTRY_POINT, STACK_TOP, 0, 0);
  log_info("unicorn engine启动完成");
  return 0;
}
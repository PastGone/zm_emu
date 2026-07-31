#include "./emu.h"
#include "./hook.h"
#include "./log/log.h"
#include "./tool/uc_helper.h"
#include "./zmaee/fs/zm_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------- 全局变量定义 -------------------- */
uc_engine *uc;
AppletHeader header;
uint32_t heap_ptr = HEAP_BASE;

uint32_t g_instance = 0;
uint32_t g_handler = 0;
int g_trap_pause = 0;
int g_disasm = 0;

csh cs_handle;
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

/* 00000405.app 新增 shim 对象地址（0x800 起，与 DUMMY_BUF@0x750 不冲突） */
uint32_t INIT_CTX = SHIM_BASE + 0x800; /* 256B 零填充：init 事件 r3 上下文 */
uint32_t SVC04 = SHIM_BASE + 0x900;    /* 0x1000004 服务对象 */
uint32_t SVC04_VT = SHIM_BASE + 0x910;
uint32_t SVC09 = SHIM_BASE + 0x940; /* 0x1000009 服务对象 */
uint32_t SVC09_VT = SHIM_BASE + 0x950;
uint32_t CBK_OBJ = SHIM_BASE + 0x980;    /* sub_84E04 返回的回调对象 */
uint32_t CBK_OBJ_VT = SHIM_BASE + 0x990; /* 可写：applet 覆写 vt[+8] */
uint32_t DLL_OBJ = SHIM_BASE + 0x9C0;    /* loadDLL 返回的 stub DLL 对象 */
uint32_t DLL_OBJ_VT = SHIM_BASE + 0x9D0;

//
uint32_t SIZE_SLOT = SHIM_BASE + 0x700; // 其实这个文件大小槽还有待确认
uint32_t API_SLOT = SHIM_BASE + 0x710;

/* -------------------- 实现 -------------------- */

int zm_emu_map_memory(uc_engine *uc) {
  uc_err err;
  err = uc_mem_map(uc, BLOB_BASE, BLOB_SIZE, UC_PROT_ALL);
  err = uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  err = uc_mem_map(uc, HEAP_BASE, HEAP_SIZE, UC_PROT_ALL);
  err = uc_mem_map(uc, SHIM_BASE, SHIM_SIZE, UC_PROT_ALL);
  err = uc_mem_map(uc, TRAMP_BASE, TRAMP_SIZE, UC_PROT_ALL);
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
  err = uc_write32(uc, ROOT, TR_root_queryRuntime);
  err = uc_write32(uc, ROOT + 0x008, TR_root_malloc);
  err = uc_write32(uc, ROOT + 0x00C, TR_root_free);
  err = uc_write32(uc, ROOT + 0x020, TR_root_str_copy);
  err = uc_write32(uc, ROOT + 0x06C, TR_root_sprintf);
  err = uc_write32(uc, ROOT + 0x088, TR_root_str_ctor);
  err = uc_write32(uc, ROOT + 0x0A4, TR_root_spec_lookup);
  err = uc_write32(uc, ROOT + 0x0A8, TR_root_str_find);

  // runtime
  err = uc_mem_write(uc, RUNTIME, &RT_VT, 4);
  err = uc_write32(uc, RT_VT + 0x08, TR_rt_queryInterface);
  err = uc_write32(uc, RT_VT + 0x10, TR_rt_getSystemInfo);
  // gfx
  err = uc_write32(uc, GFX, GFX_VT);
  err = uc_write32(uc, GFX_VT + 0x04, TR_svc_release);
  err = uc_write32(uc, GFX_VT + 0x20, TR_gfx_clear);
  err = uc_write32(uc, GFX_VT + 0x2C, TR_gfx_fillRect);
  err = uc_write32(uc, GFX_VT + 0x40, TR_gfx_commit);
  err = uc_write32(uc, GFX_VT + 0x50, TR_gfx_drawText);
  err = uc_write32(uc, GFX_VT + 0x6C, TR_gfx_drawRect);
  err = uc_write32(uc, GFX_VT + 0x70, TR_gfx_fillRect2);

  /* FS_VT[0x30]：enumFile — sub_82584 枚举 app_list 下文件 */
  err = uc_write32(uc, FS_VT + 0x30, TR_fs_enum);

  // fs
  err = uc_write32(uc, FS, FS_VT);
  err = uc_write32(uc, FS_VT + 0x08, TR_fs_open);
  err = uc_write32(uc, FILE1, FILE_VT);
  err = uc_write32(uc, FILE_VT + 0x04, TR_file_close);
  err = uc_write32(uc, FILE_VT + 0x08, TR_file_read);
  err = uc_write32(uc, FILE_VT + 0x20, TR_file_seek);
  err = uc_write32(uc, FILE_VT + 0x24, TR_file_size);
  // audio
  err = uc_write32(uc, AUDIO, AUDIO_VT);
  err = uc_write32(uc, AUDIO_VT + 0x04, TR_svc_release);
  err = uc_write32(uc, AUDIO_VT + 0x14, TR_audio_stop);
  err = uc_write32(uc, AUDIO_VT + 0x24, TR_audio_get_status);
  err = uc_write32(uc, AP, AP_VT);
  err = uc_write32(uc, AP_VT + 0x04, TR_svc_release);
  err = uc_write32(uc, AP_VT + 0x10, TR_ap_play);
  err = uc_write32(uc, AP_VT + 0x14, TR_ap_stop);

  /* ---- 00000405.app：补全 ROOT vtable 缺失槽（.lst 已验证偏移） ---- */

  err = uc_write32(uc, ROOT + 0x060, TR_root_memset);
  err = uc_write32(uc, ROOT + 0x074, TR_root_x74);
  err = uc_write32(uc, ROOT + 0x078, TR_root_str_assign);
  err = uc_write32(uc, ROOT + 0x0D8, TR_root_get_tick);
  err = uc_write32(uc, ROOT + 0x12C, TR_root_x12C);
  err = uc_write32(uc, ROOT + 0x130, TR_root_x130);
  err = uc_write32(uc, ROOT + 0x140, TR_root_x140);
  err = uc_write32(uc, ROOT + 0x154, TR_root_create_cbk);
  err = uc_write32(uc, ROOT + 0x16C, TR_root_x16C);

  /* ---- 新增 runtime 服务对象 SVC04 / SVC09 ---- */
  err = uc_write32(uc, SVC04, SVC04_VT);
  err = uc_write32(uc, SVC04_VT + 0x04, TR_svc_release);
  err = uc_write32(uc, SVC04_VT + 0x1C, TR_svc04_x1C);
  err = uc_write32(uc, SVC09, SVC09_VT);
  err = uc_write32(uc, SVC09_VT + 0x04, TR_svc_release);
  err = uc_write32(uc, SVC09_VT + 0x2C, TR_svc09_x2C);
  err = uc_write32(uc, SVC09_VT + 0x40, TR_svc09_x40);

  /* ---- FS 补 release 与 chdir 槽 ---- */
  err = uc_write32(uc, FS_VT + 0x04, TR_fs_release);
  err = uc_write32(uc, FS_VT + 0x14, TR_fs_chdir);

  /* ---- RUNTIME 补 loadDLL / unloadDLL / loadDLL2 ---- */
  err = uc_write32(uc, RT_VT + 0x58, TR_rt_loadDLL);
  err = uc_write32(uc, RT_VT + 0x5C, TR_rt_unloadDLL);
  err = uc_write32(uc, RT_VT + 0x78, TR_rt_loadDLL2);

  /* ---- CBK 回调对象（sub_84E04 返回，vt[+8] 会被 applet 覆写为 sub_82FF8）
   * ---- */
  err = uc_write32(uc, CBK_OBJ, CBK_OBJ_VT);
  err = uc_write32(uc, CBK_OBJ_VT + 0x08, TR_cbk_default);

  /* ---- stub DLL 对象（loadDLL 返回） ---- */
  err = uc_write32(uc, DLL_OBJ, DLL_OBJ_VT);
  err = uc_write32(uc, DLL_OBJ_VT + 0x08, TR_dll_init);
  err = uc_write32(uc, DLL_OBJ_VT + 0x0C, TR_dll_config);
  err = uc_write32(uc, DLL_OBJ_VT + 0x10, TR_dll_entry);

  /* INIT_CTX 显式零填充（Unicorn 默认零，此处双保险，确保 r3+0x100 可读） */
  {
    uint8_t zeros[256] = {0};
    err = uc_mem_write(uc, INIT_CTX, zeros, sizeof(zeros));
  }

  if (err != UC_ERR_OK) {
    log_error("uc_mem_write failed, err: %d\n", err);
    return -1;
  }
  log_info("虚表构建完成");
  return 0;
}

int zm_emu_load_blob(uc_engine *uc, FILE *fp, long *applet_size) {

  log_info("开始载入blob数据");
  unsigned char *buf = malloc(*applet_size);
  if (buf == NULL) {
    log_error("malloc failed");
    fclose(fp);
    return -1;
  }

  size_t bytes_read = fread(buf, 1, *applet_size, fp);
  if (bytes_read != (size_t)*applet_size) {
    log_error("读取文件失败，期望%zu字节，实际读取%zu", (size_t)*applet_size,
              bytes_read);
    free(buf);
    fclose(fp);
    return -1;
  }

  uc_err err = uc_mem_write(uc, BLOB_BASE, buf, *applet_size);
  if (err != UC_ERR_OK) {
    log_error("uc_mem_write failed, err: %d\n", err);
    free(buf);
    fclose(fp);
    return -1;
  }
  free(buf);
  fclose(fp);
  log_info("blob数据载入完成,文件流已被关闭");

  return 0;
}

int zm_emu_add_hooks(uc_engine *uc) {
  uc_hook hook_code_handle;
  uc_hook hook_unmapped_mem_handle;
  uc_hook hook_shim_mem_handle;

  uc_err err;
  log_info("添加钩子");

  err = uc_hook_add(uc, &hook_code_handle, UC_HOOK_CODE, (void *)hook_code,
                    NULL, 1, 0);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add failed, err: %d\n", err);
    return -1;
  }

  err = uc_hook_add(uc, &hook_unmapped_mem_handle, UC_HOOK_MEM_UNMAPPED,
                    (void *)hook_mem_unmapped, NULL, 1, 0);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add failed, err: %d\n", err);
    return -1;
  }

  err = uc_hook_add(uc, &hook_shim_mem_handle,
                    UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, (void *)hook_shim_mem,
                    NULL, SHIM_BASE, SHIM_BASE + SHIM_SIZE);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add failed, err: %d\n", err);
    return -1;
  }
  log_info("钩子添加完成");
  return 0;
}

int zm_emu_start_applet(uc_engine *uc) {
  uint32_t stack_ptr = STACK_TOP;
  uc_reg_write(uc, UC_ARM_REG_SP, &stack_ptr);

  uc_reg_write(
      uc, UC_ARM_REG_LR,
      &(uint32_t){TR_init_callback}); // LR 是返回地址，这里写入初始化回调
  uc_reg_write(uc, UC_ARM_REG_R0, &SIZE_SLOT);
  uc_reg_write(uc, UC_ARM_REG_R1, &API_SLOT);

  uc_write32(uc, BLOB_BASE + ROOT_SLOT_OFF, (uint32_t)ROOT);

  log_info("启动unicorn engine...");
  uc_emu_start(uc, APPLET_ENTRY_POINT, STACK_TOP, 0, 0);
  log_info("unicorn engine启动完成");
  return 0;
}

#include "./emu.h"
#include "./hook.h"
#include "./log/log.h"
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
uint32_t TR_file_size = TRAP(58); /* FILE_VT[0x24]：返回文件总大小 */

uint32_t TR_audio_stop = TRAP(20);
uint32_t TR_ap_play = TRAP(21);
uint32_t TR_ap_stop = TRAP(22);
uint32_t TR_audio_get_status = TRAP(60); /* AUDIO_VT[0x24] */

/* 00000405.app：GFX vtable 缺失槽（索引 61..70） */
uint32_t TR_gfx_x18 = TRAP(61); /* GFX_VT[0x18] */
uint32_t TR_gfx_x34 = TRAP(62); /* GFX_VT[0x34] */
uint32_t TR_gfx_x38 = TRAP(63); /* GFX_VT[0x38] */
uint32_t TR_gfx_x44 = TRAP(64); /* GFX_VT[0x44] */
uint32_t TR_gfx_x48 = TRAP(65); /* GFX_VT[0x48] */
uint32_t TR_gfx_x54 = TRAP(66); /* GFX_VT[0x54] */
uint32_t TR_gfx_x68 = TRAP(67); /* GFX_VT[0x68] */
uint32_t TR_gfx_x94 = TRAP(68); /* GFX_VT[0x94] */
uint32_t TR_gfx_xA4 = TRAP(69); /* GFX_VT[0xA4] */
uint32_t TR_gfx_xB0 = TRAP(70); /* GFX_VT[0xB0] */

/* 00000405.app：GFX vtable 补充缺失槽（索引 71..73） */
uint32_t TR_gfx_x0C = TRAP(71); /* GFX_VT[0x0C] */
uint32_t TR_gfx_x28 = TRAP(72); /* GFX_VT[0x28] */
uint32_t TR_gfx_x4C = TRAP(73); /* GFX_VT[0x4C] */

/* 00000405.app：FS vtable 缺失槽 */
uint32_t TR_fs_enum = TRAP(74); /* FS_VT[0x30]：enumFile */

uint32_t TR_init_callback =
    TRAP(114514); // 随便写一个位置我想也应该不影响这个叫什么来.初始化回调

uint32_t SIZE_SLOT = SHIM_BASE + 0x700; // 其实这个文件大小槽还有待确认
uint32_t API_SLOT = SHIM_BASE + 0x710;

/* 00000405.app 新增 ROOT vtable trap（索引 23..45） */
uint32_t TR_root_x18 = TRAP(23);
uint32_t TR_root_x1C = TRAP(24);
uint32_t TR_root_x24 = TRAP(25);
uint32_t TR_root_x50 = TRAP(26);
uint32_t TR_root_x5C = TRAP(27);
uint32_t TR_root_memset = TRAP(28); /* ROOT[0x60] */
uint32_t TR_root_x74 = TRAP(29);
uint32_t TR_root_str_assign = TRAP(30); /* ROOT[0x78] */
uint32_t TR_root_x7C = TRAP(31);
uint32_t TR_root_x80 = TRAP(32);
uint32_t TR_root_x84 = TRAP(33);
uint32_t TR_root_x8C = TRAP(34);
uint32_t TR_root_x90 = TRAP(35);
uint32_t TR_root_xB0 = TRAP(36);
uint32_t TR_root_xC0 = TRAP(37);
uint32_t TR_root_xC8 = TRAP(38);
uint32_t TR_root_xD0 = TRAP(39);
uint32_t TR_root_get_tick = TRAP(40); /* ROOT[0xD8] */
uint32_t TR_root_x12C = TRAP(41);
uint32_t TR_root_x130 = TRAP(42);
uint32_t TR_root_x140 = TRAP(43);
uint32_t TR_root_create_cbk = TRAP(44); /* ROOT[0x154] */
uint32_t TR_root_x16C = TRAP(45);

/* 服务对象 / FS / RT / DLL / CBK trap（索引 46..57） */
uint32_t TR_svc_release = TRAP(46);
uint32_t TR_svc04_x1C = TRAP(47);
uint32_t TR_svc09_x2C = TRAP(48);
uint32_t TR_svc09_x40 = TRAP(49);
uint32_t TR_fs_release = TRAP(50);
uint32_t TR_fs_chdir = TRAP(51);
uint32_t TR_rt_loadDLL = TRAP(52);
uint32_t TR_rt_unloadDLL = TRAP(53);
uint32_t TR_rt_loadDLL2 = TRAP(59); /* RT_VT[0x78] */
uint32_t TR_dll_init = TRAP(54);
uint32_t TR_dll_config = TRAP(55);
uint32_t TR_dll_entry = TRAP(56);
uint32_t TR_cbk_default = TRAP(57);

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
  err = uc_mem_write(uc, GFX_VT + 0x04, &TR_svc_release, 4);
  err = uc_mem_write(uc, GFX_VT + 0x20, &TR_gfx_clear, 4);
  err = uc_mem_write(uc, GFX_VT + 0x2C, &TR_gfx_fillRect, 4);
  err = uc_mem_write(uc, GFX_VT + 0x40, &TR_gfx_commit, 4);
  err = uc_mem_write(uc, GFX_VT + 0x50, &TR_gfx_drawText, 4);
  err = uc_mem_write(uc, GFX_VT + 0x6C, &TR_gfx_drawRect, 4);
  err = uc_mem_write(uc, GFX_VT + 0x70, &TR_gfx_fillRect2, 4);

  /* 00000405.app：补全 GFX vtable 缺失槽 */
  err = uc_mem_write(uc, GFX_VT + 0x18, &TR_gfx_x18, 4);
  err = uc_mem_write(uc, GFX_VT + 0x34, &TR_gfx_x34, 4);
  err = uc_mem_write(uc, GFX_VT + 0x38, &TR_gfx_x38, 4);
  err = uc_mem_write(uc, GFX_VT + 0x44, &TR_gfx_x44, 4);
  err = uc_mem_write(uc, GFX_VT + 0x48, &TR_gfx_x48, 4);
  err = uc_mem_write(uc, GFX_VT + 0x54, &TR_gfx_x54, 4);
  err = uc_mem_write(uc, GFX_VT + 0x68, &TR_gfx_x68, 4);
  err = uc_mem_write(uc, GFX_VT + 0x94, &TR_gfx_x94, 4);
  err = uc_mem_write(uc, GFX_VT + 0xA4, &TR_gfx_xA4, 4);
  err = uc_mem_write(uc, GFX_VT + 0xB0, &TR_gfx_xB0, 4);
  err = uc_mem_write(uc, GFX_VT + 0x0C, &TR_gfx_x0C, 4);
  err = uc_mem_write(uc, GFX_VT + 0x28, &TR_gfx_x28, 4);
  err = uc_mem_write(uc, GFX_VT + 0x4C, &TR_gfx_x4C, 4);
  /* FS_VT[0x30]：enumFile — sub_82584 枚举 app_list 下文件 */
  err = uc_mem_write(uc, FS_VT + 0x30, &TR_fs_enum, 4);

  // fs
  err = uc_mem_write(uc, FS, &FS_VT, 4);
  err = uc_mem_write(uc, FS_VT + 0x08, &TR_fs_open, 4);
  err = uc_mem_write(uc, FILE1, &FILE_VT, 4);
  err = uc_mem_write(uc, FILE_VT + 0x04, &TR_file_close, 4);
  err = uc_mem_write(uc, FILE_VT + 0x08, &TR_file_read, 4);
  err = uc_mem_write(uc, FILE_VT + 0x20, &TR_file_seek, 4);
  err = uc_mem_write(uc, FILE_VT + 0x24, &TR_file_size, 4);
  // audio
  err = uc_mem_write(uc, AUDIO, &AUDIO_VT, 4);
  err = uc_mem_write(uc, AUDIO_VT + 0x04, &TR_svc_release, 4);
  err = uc_mem_write(uc, AUDIO_VT + 0x14, &TR_audio_stop, 4);
  err = uc_mem_write(uc, AUDIO_VT + 0x24, &TR_audio_get_status, 4);
  err = uc_mem_write(uc, AP, &AP_VT, 4);
  err = uc_mem_write(uc, AP_VT + 0x04, &TR_svc_release, 4);
  err = uc_mem_write(uc, AP_VT + 0x10, &TR_ap_play, 4);
  err = uc_mem_write(uc, AP_VT + 0x14, &TR_ap_stop, 4);

  /* ---- 00000405.app：补全 ROOT vtable 缺失槽（.lst 已验证偏移） ---- */
  err = uc_mem_write(uc, ROOT + 0x018, &TR_root_x18, 4);
  err = uc_mem_write(uc, ROOT + 0x01C, &TR_root_x1C, 4);
  err = uc_mem_write(uc, ROOT + 0x024, &TR_root_x24, 4);
  err = uc_mem_write(uc, ROOT + 0x050, &TR_root_x50, 4);
  err = uc_mem_write(uc, ROOT + 0x05C, &TR_root_x5C, 4);
  err = uc_mem_write(uc, ROOT + 0x060, &TR_root_memset, 4);
  err = uc_mem_write(uc, ROOT + 0x074, &TR_root_x74, 4);
  err = uc_mem_write(uc, ROOT + 0x078, &TR_root_str_assign, 4);
  err = uc_mem_write(uc, ROOT + 0x07C, &TR_root_x7C, 4);
  err = uc_mem_write(uc, ROOT + 0x080, &TR_root_x80, 4);
  err = uc_mem_write(uc, ROOT + 0x084, &TR_root_x84, 4);
  err = uc_mem_write(uc, ROOT + 0x08C, &TR_root_x8C, 4);
  err = uc_mem_write(uc, ROOT + 0x090, &TR_root_x90, 4);
  err = uc_mem_write(uc, ROOT + 0x0B0, &TR_root_xB0, 4);
  err = uc_mem_write(uc, ROOT + 0x0C0, &TR_root_xC0, 4);
  err = uc_mem_write(uc, ROOT + 0x0C8, &TR_root_xC8, 4);
  err = uc_mem_write(uc, ROOT + 0x0D0, &TR_root_xD0, 4);
  err = uc_mem_write(uc, ROOT + 0x0D8, &TR_root_get_tick, 4);
  err = uc_mem_write(uc, ROOT + 0x12C, &TR_root_x12C, 4);
  err = uc_mem_write(uc, ROOT + 0x130, &TR_root_x130, 4);
  err = uc_mem_write(uc, ROOT + 0x140, &TR_root_x140, 4);
  err = uc_mem_write(uc, ROOT + 0x154, &TR_root_create_cbk, 4);
  err = uc_mem_write(uc, ROOT + 0x16C, &TR_root_x16C, 4);

  /* ---- 新增 runtime 服务对象 SVC04 / SVC09 ---- */
  err = uc_mem_write(uc, SVC04, &SVC04_VT, 4);
  err = uc_mem_write(uc, SVC04_VT + 0x04, &TR_svc_release, 4);
  err = uc_mem_write(uc, SVC04_VT + 0x1C, &TR_svc04_x1C, 4);
  err = uc_mem_write(uc, SVC09, &SVC09_VT, 4);
  err = uc_mem_write(uc, SVC09_VT + 0x04, &TR_svc_release, 4);
  err = uc_mem_write(uc, SVC09_VT + 0x2C, &TR_svc09_x2C, 4);
  err = uc_mem_write(uc, SVC09_VT + 0x40, &TR_svc09_x40, 4);

  /* ---- FS 补 release 与 chdir 槽 ---- */
  err = uc_mem_write(uc, FS_VT + 0x04, &TR_fs_release, 4);
  err = uc_mem_write(uc, FS_VT + 0x14, &TR_fs_chdir, 4);

  /* ---- RUNTIME 补 loadDLL / unloadDLL / loadDLL2 ---- */
  err = uc_mem_write(uc, RT_VT + 0x58, &TR_rt_loadDLL, 4);
  err = uc_mem_write(uc, RT_VT + 0x5C, &TR_rt_unloadDLL, 4);
  err = uc_mem_write(uc, RT_VT + 0x78, &TR_rt_loadDLL2, 4);

  /* ---- CBK 回调对象（sub_84E04 返回，vt[+8] 会被 applet 覆写为 sub_82FF8）
   * ---- */
  err = uc_mem_write(uc, CBK_OBJ, &CBK_OBJ_VT, 4);
  err = uc_mem_write(uc, CBK_OBJ_VT + 0x08, &TR_cbk_default, 4);

  /* ---- stub DLL 对象（loadDLL 返回） ---- */
  err = uc_mem_write(uc, DLL_OBJ, &DLL_OBJ_VT, 4);
  err = uc_mem_write(uc, DLL_OBJ_VT + 0x08, &TR_dll_init, 4);
  err = uc_mem_write(uc, DLL_OBJ_VT + 0x0C, &TR_dll_config, 4);
  err = uc_mem_write(uc, DLL_OBJ_VT + 0x10, &TR_dll_entry, 4);

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
  uc_reg_write(uc, UC_ARM_REG_LR,
               &TR_init_callback); // LR 是返回地址，这里写入初始化回调
  uc_reg_write(uc, UC_ARM_REG_R0, &SIZE_SLOT);
  uc_reg_write(uc, UC_ARM_REG_R1, &API_SLOT);

  uc_mem_write(uc, BLOB_BASE + ROOT_SLOT_OFF, &ROOT, 4);

  log_info("启动unicorn engine...");
  uc_emu_start(uc, APPLET_ENTRY_POINT, STACK_TOP, 0, 0);
  log_info("unicorn engine启动完成");
  return 0;
}

void zm_emu_load_zmr_if_exists(uc_engine *uc, const char *app_path) {
  (void)uc;
  char zmr_path[1024];
  strncpy(zmr_path, app_path, sizeof(zmr_path) - 1);
  zmr_path[sizeof(zmr_path) - 1] = '\0';
  size_t plen = strlen(zmr_path);
  if (plen >= 4 && strcmp(zmr_path + plen - 4, ".app") == 0) {
    strcpy(zmr_path + plen - 4, ".zmr");
  } else {
    strncat(zmr_path, ".zmr", sizeof(zmr_path) - plen - 1);
  }

  if (!zm_fs_load_zmr(zmr_path)) {
    log_warn("未找到或无法载入 .zmr 资源: %s", zmr_path);
  }
}
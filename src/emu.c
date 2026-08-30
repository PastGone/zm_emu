#include "./emu.h"
#include "./hook.h"
#include "./log/log.h"
#include "./tool/uc_helper.h"
#include "./ulibc/ulibc.h"
#include "./zmaee/fs/zm_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------- 全局变量定义 -------------------- */
uc_engine *g_uc;
AppletHeader g_header;
uint32_t g_heap_ptr = HEAP_BASE;
int g_ulibc_heap = 1;

uint32_t g_instance = 0;
uint32_t g_handler = 0;
int g_trap_pause = 0;
int g_disasm = 1;

csh g_cs_handle;
cs_insn *g_sc_insn;
size_t g_sc_count;
uint8_t g_cscode[16];

/* -------------------- 实现 -------------------- */

int zm_emu_map_memory() {
  uc_err err;
  err = uc_mem_map(g_uc, BLOB_BASE, BLOB_SIZE, UC_PROT_ALL);
  err = uc_mem_map(g_uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  err = uc_mem_map(g_uc, HEAP_BASE, HEAP_SIZE, UC_PROT_ALL);
  err = uc_mem_map(g_uc, SHIM_BASE, SHIM_SIZE, UC_PROT_ALL);
  err = uc_mem_map(g_uc, TRAMP_BASE, TRAMP_SIZE, UC_PROT_ALL);
  if (err != UC_ERR_OK) {
    log_error("uc_mem_map failed, err: %d\n", err);
    return -1;
  }

  /*
   * 客户机堆策略（见 emu.h 中 g_ulibc_heap 的说明）。
   *
   * 默认使用 ulibc 真实堆：malloc/free 真正回收复用，与原始固件行为一致。
   * ZM_ULIBC_HEAP=0 可回退到 bump 分配器，仅用于排查 applet 的
   * UAF / double-free（bump 下 free 是空操作，能掩盖这类错误）。
   */
  {
    const char *env = getenv("ZM_ULIBC_HEAP");
    g_ulibc_heap = (env && strcmp(env, "0") == 0) ? 0 : 1;
  }
  u_heap_init(g_uc, HEAP_BASE, HEAP_SIZE);
  log_info("内存映射完成（客户机堆：%s，%u 字节）",
           g_ulibc_heap ? "ulibc 真实堆" : "bump 分配器（排查用）", HEAP_SIZE);
  return 0;
}

int zm_emu_build_vtables() {
  uc_err err;
  // shim 和 tramp 是对射关系,先假设它全部是这样然后后面再做修补修改
  // 假设它全部是函数指针实际上是有对象的后面会进行修补
  // 一个函数指针是四字节所以这里是加四字节
  for (uint32_t i = 0; i < SHIM_SIZE; i += 4) {
    err = uc_write32(g_uc, SHIM_BASE + i, TRAMP_BASE + i);
    if (err != UC_ERR_OK) {
      log_error("shim映射到tramp时出现了错误, err: %d\n", err);
      return -1;
    }
  }
  // root
  err = uc_write32(g_uc, ROOT, TR_root_getShell);

  // runtime
  err = uc_write32(g_uc, SHELL, RT_VT);

  // gfx
  err = uc_write32(g_uc, GFX, GFX_VT);

  /* FileMgr_VT[0x30]：enumFile — sub_82584 枚举 app_list 下文件 */
  // err = uc_write32(g_uc, FileMgr_VT + 0x30, TR_fileMgr_enum);

  // fs
  err = uc_write32(g_uc, FileMgr, FileMgr_VT);
  err = uc_write32(g_uc, FILE1, FILE_VT);

  // audio
  err = uc_write32(g_uc, AUDIO, AUDIO_VT);
  err = uc_write32(g_uc, AP, AP_VT);

  /* ---- 新增 runtime 服务对象 SVC04 / SVC09 ---- */
  err = uc_write32(g_uc, SVC04, SVC04_VT);

  /* ---- CBK 回调对象（sub_84E04 返回，vt[+8] 会被 applet 覆写为 sub_82FF8）
   * ---- */
  err = uc_write32(g_uc, CBK_OBJ, CBK_OBJ_VT);
  // err = uc_write32(g_uc, CBK_OBJ_VT + 0x08, TR_cbk_default);

  /* ---- stub DLL 对象（loadDLL 返回） ---- */
  err = uc_write32(g_uc, DLL_OBJ, DLL_OBJ_VT);

  /* INIT_CTX 显式零填充（Unicorn 默认零，此处双保险，确保 r3+0x100 可读） */
  {
    uint8_t zeros[256] = {0};
    err = uc_mem_write(g_uc, INIT_CTX, zeros, sizeof(zeros));
  }

  if (err != UC_ERR_OK) {
    log_error("uc_mem_write failed, err: %d\n", err);
    return -1;
  }
  log_info("虚表构建完成");
  return 0;
}

int zm_emu_load_blob(FILE *fp, const long *applet_size) {

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

  uc_err err = uc_mem_write(g_uc, BLOB_BASE, buf, *applet_size);
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

int zm_emu_add_hooks() {
  uc_hook hook_code_handle;
  uc_hook hook_unmapped_mem_handle;
  uc_hook hook_shim_mem_handle;

  uc_err err;
  log_info("添加钩子");
  err = uc_hook_add(g_uc, &hook_code_handle, UC_HOOK_CODE, (void *)hook_code,
                    NULL, 1, 0);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add failed, err: %d\n", err);
    return -1;
  }

  err = uc_hook_add(g_uc, &hook_unmapped_mem_handle, UC_HOOK_MEM_UNMAPPED,
                    (void *)hook_mem_unmapped, NULL, 1, 0);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add failed, err: %d\n", err);
    return -1;
  }

  err = uc_hook_add(g_uc, &hook_shim_mem_handle,
                    UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, (void *)hook_shim_mem,
                    NULL, SHIM_BASE, SHIM_BASE + SHIM_SIZE);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add failed, err: %d\n", err);
    return -1;
  }
  log_info("钩子添加完成");
  return 0;
}

int zm_emu_start_applet() {
  uint32_t stack_ptr = STACK_TOP;
  uc_reg_write(g_uc, UC_ARM_REG_SP, &stack_ptr);

  uc_reg_write(
      g_uc, UC_ARM_REG_LR,
      &(uint32_t){TR_init_callback}); // LR 是返回地址，这里写入初始化回调
  uc_reg_write(g_uc, UC_ARM_REG_R0, &(uint32_t){SIZE_SLOT});
  uc_reg_write(g_uc, UC_ARM_REG_R1, &(uint32_t){API_SLOT});

  uc_write32(g_uc, BLOB_BASE + ROOT_SLOT_OFF, (uint32_t)ROOT);

  log_info("启动unicorn engine...");
  uc_emu_start(g_uc, APPLET_ENTRY_POINT, STACK_TOP, 0, 0);
  log_info("unicorn engine启动完成");
  return 0;
}

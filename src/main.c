// 测试主函数
//
#include "./log/log.h"
//
#include "./emu.h"
#include "./test/test_diag.h"
#include "./tool/odds.h"
#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/fs/zm_fs.h"
#include "./zmaee/gfx/zm_gfx.h"
//
#include <SDL2/SDL.h>
#include <capstone/capstone.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

int main() {
  log_info("hello world!");

  // 调试开关：ZM_STEP=1 每个 trap 后等待回车；ZM_DISASM=1 反汇编每条指令
  {
    const char *s = getenv("ZM_STEP");
    if (s && *s)
      g_trap_pause = 1;
    s = getenv("ZM_DISASM");
    if (s && *s)
      g_disasm = 1;
  }
  // g_trap_pause = 1;
  // g_disasm = 1; // 用 ZM_DISASM=1 环境变量开启

  // 初始化 Capstone，使用 ARM-32 架构（CS_ARCH_ARM，CS_MODE_ARM）
  if (cs_open(CS_ARCH_ARM, CS_MODE_ARM, &cs_handle) != CS_ERR_OK) {
    fprintf(stderr, "Failed to open Capstone\n");
    return 1;
  }

  uc_err my_uc_err;

  // 打开 applet 文件
  char *filename = "/home/apollo/文档/古时游戏/zm_emu/applet/00000102/"
                   "00000102.app"; // 测试文件1（向后兼容验证）
  // char *filename = "/home/apollo/文档/古时游戏/zm_emu/applet/00000405/"
  //                  "00000405.app"; // 测试文件2（号码归属）
  // /home/apollo/文档/古时游戏/zmaee_emu/zemee/0000050c/0000050c.app
  // char *filename = "/home/apollo/文档/古时游戏/zmaee_emu/zemee/0000050c/"
  //                  "0000050c.app"; // 测试文件3

  // 先解析 applet 头（不依赖 uc），以获取屏幕尺寸
  FILE *fp = fopen(filename, "rb");
  long applet_size;

  {
    if (fp == NULL) {
      log_error("fopen failed");
      cs_close(&cs_handle);
      return 1;
    }
    applet_size = get_file_size(fp);
    log_info("文件大小: %ld\n", applet_size);
    parse_app_header(fp, &header);
    print_header(&header);
  }

  // 初始化 SDL2 渲染与音频

  if (zm_gfx_init() != 0) {
    log_warn("zm_gfx_init 失败，渲染将不可用（继续运行）");
  }
  if (zm_audio_init() != 0) {
    log_warn("zm_audio_init 失败，音频将不可用（继续运行）");
  }

  // 初始化 unicorn 引擎
  {
    my_uc_err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (my_uc_err != UC_ERR_OK) {
      log_error("uc_open failed, err: %d\n", my_uc_err);
      cs_close(&cs_handle);
      fclose(fp);
      return 1;
    }
    log_info("unicorn engine initialized");
  }

  // 内存映射
  if (zm_emu_map_memory(uc) != 0) {
    uc_close(uc);
    cs_close(&cs_handle);
    fclose(fp);
    return 1;
  }

  // 构建虚表
  if (zm_emu_build_vtables(uc) != 0) {
    uc_close(uc);
    cs_close(&cs_handle);
    fclose(fp);
    return 1;
  }

  // 注册钩子

  if (zm_emu_add_hooks(uc) != 0) {
    uc_close(uc);
    cs_close(&cs_handle);
    fclose(fp);
    return 1;
  }

  // 载入 blob 数据到客户机内存, 并关闭文件
  {
    if (zm_emu_load_blob(uc, fp, &applet_size) != 0) {
      uc_close(uc);
      cs_close(&cs_handle);
      return 1;
    }
  }

  // 载入 .zmr 资源
  // zm_emu_load_zmr_if_exists(uc, filename);

  // // 登记多文件 fs 表（00000405.app：config.b / zmsys006.dll / 图标等）
  // // 从 filename 取目录部分传给 zm_fs_register_default
  // {
  //   char applet_dir[1024];
  //   strncpy(applet_dir, filename, sizeof(applet_dir) - 1);
  //   applet_dir[sizeof(applet_dir) - 1] = '\0';
  //   char *slash = strrchr(applet_dir, '/');
  //   if (slash)
  //     *slash = '\0';
  //   zm_fs_register_default(applet_dir);
  //   // 登记 applet 自身（applet 可能通过 sprintf("%s%08x.app") 打开）
  //   zm_fs_register_hostfile(filename, "00000405.app");
  // }

  // 启动 applet（init → 绘制 → 停止）
  zm_emu_start_applet(uc);

  // 调试 & 自测（已拆至 test/test_diag.c）//diag 的意思是诊断
  zm_diag_dump_buttons(uc);
  zm_diag_audio_test(uc);
  zm_diag_auto_click();
  zm_diag_run_event_loop();

  // 释放资源
  zm_audio_shutdown();
  zm_gfx_shutdown();
  zm_fs_shutdown();
  cs_close(&cs_handle);
  uc_close(uc);

  return 0;
}
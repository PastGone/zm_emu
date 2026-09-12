// 测试主函数
//
#include "./log/log.h"
//
#include "./emu.h"
#include "./test/test_diag.h"
#include "./tool/odds.h"
#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/fs/zm_file_mgr.h"
#include "./zmaee/fs/zm_file.h"
#include "./zmaee/gfx/zm_display.h"
//
#include <SDL2/SDL.h>
#include <capstone/capstone.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

/* 当前 applet 短名称，供 trap.c 写入 instance+4 */
char g_app_pathname[4096] = {0};

int main() {
  /* ---------------- 日志开关 ----------------
   * ZM_LOG=off   关闭全部日志（屏幕 + log.txt）—— 肉眼验证画面时用这个
   * ZM_LOG=error 只看错误   ZM_LOG=warn / info / debug 依次放宽
   * 不设该变量 = 全开（LOG_TRACE），与历史行为一致。
   *
   * 全量日志会明显拖慢模拟（HOOK 单次运行就能刷出 7000+ 行），
   * 而且每行都要同时格式化到屏幕和 log.txt 两次。 */
  int log_level = LOG_TRACE;
  bool log_quiet = false;
  {
    const char *lv = getenv("ZM_LOG");
    if (lv && lv[0]) {
      if (!strcmp(lv, "off") || !strcmp(lv, "none")) {
        log_level = LOG_FATAL; /* 回调阈值顺便抬高，确保不动文件 */
        log_quiet = true;
      } else if (!strcmp(lv, "error")) {
        log_level = LOG_ERROR;
      } else if (!strcmp(lv, "warn")) {
        log_level = LOG_WARN;
      } else if (!strcmp(lv, "info")) {
        log_level = LOG_INFO;
      } else if (!strcmp(lv, "debug")) {
        log_level = LOG_DEBUG;
      }
    }
  }
  log_set_level(log_level);
  log_set_quiet(log_quiet);
  log_info("hello world!");

  /* 关日志时连 log.txt 也不开、不注册回调（否则回调仍会写盘）。 */
  FILE *logfile = NULL;
  if (!log_quiet) {
    logfile = fopen("log.txt", "w");
    log_add_fp(logfile, log_level);
  }

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
  if (cs_open(CS_ARCH_ARM, CS_MODE_ARM, &g_cs_handle) != CS_ERR_OK) {
    fprintf(stderr, "Failed to open Capstone\n");
    return 1;
  }

  uc_err my_uc_err;

  // 打开 applet 文件
  //  测试文件1（向后兼容验证）,已测试通过
  // 整个框架就是在这个这个东西就是起点,如果这个东西不通过那就是破坏了兼容性
  char *filename = "/home/apollo/文档/古时游戏/zm_emu/applet/00000102/"
                   "00000102.app"; //

  // char *filename = "/home/apollo/文档/古时游戏/zm_emu/applet/00000001/"
  //                  "00000001.app"; // 测试文件1（向后兼容验证）

  /*
   * ZM_APPLET 可覆盖默认 applet（填 applet 目录下的短名，如 00000405，
   * 或完整路径），便于逐个回归测试。
   *
   * 注意：只能把结果写到**独立的** applet_path 缓冲，再让 filename 指向它。
   * 不要写成 snprintf(g_app_pathname, n, "%s", filename) 且 filename 已指向
   * g_app_pathname —— 那是 snprintf 自赋值，会破坏缓冲区。
   */
  {
    static char applet_path[4096];
    const char *sel = getenv("ZM_APPLET");
    if (sel && *sel) {
      if (strchr(sel, '/'))
        snprintf(applet_path, sizeof(applet_path), "%s", sel);
      else
        snprintf(applet_path, sizeof(applet_path),
                 "/home/apollo/文档/古时游戏/zm_emu/applet/%s/%s.app", sel,
                 sel);
      filename = applet_path;
    }
    snprintf(g_app_pathname, sizeof(g_app_pathname), "%s", filename);
    log_info("载入 applet: %s", filename);
  }

  // 先解析 applet 头（不依赖 g_uc），以获取屏幕尺寸
  FILE *fp = fopen(filename, "rb");
  long applet_size;

  {
    if (fp == NULL) {
      log_error("fopen failed");
      cs_close(&g_cs_handle);
      return 1;
    }
    applet_size = get_file_size(fp);
    log_info("文件大小: %ld\n", applet_size);
    parse_app_header(fp, &g_header);
    print_header(&g_header);
  }

  // 初始化 SDL2 渲染与音频

  if (zm_display_init() != 0) {
    log_warn("zm_display_init 失败，渲染将不可用（继续运行）");
  }
  if (zm_audio_init() != 0) {
    log_warn("zm_audio_init 失败，音频将不可用（继续运行）");
  }

  // 初始化 unicorn 引擎
  {
    my_uc_err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &g_uc);
    if (my_uc_err != UC_ERR_OK) {
      log_error("uc_open failed, err: %d\n", my_uc_err);
      cs_close(&g_cs_handle);
      fclose(fp);
      return 1;
    }
    log_info("unicorn engine initialized");
  }

  // 内存映射
  if (zm_emu_map_memory() != 0) {
    uc_close(g_uc);
    cs_close(&g_cs_handle);
    fclose(fp);
    return 1;
  }

  // 构建虚表
  if (zm_emu_build_vtables() != 0) {
    uc_close(g_uc);
    cs_close(&g_cs_handle);
    fclose(fp);
    return 1;
  }

  // 注册钩子

  if (zm_emu_add_hooks() != 0) {
    uc_close(g_uc);
    cs_close(&g_cs_handle);
    fclose(fp);
    return 1;
  }

  // 载入 blob 数据到客户机内存, 并关闭文件
  {
    if (zm_emu_load_blob(fp, &applet_size) != 0) {
      uc_close(g_uc);
      cs_close(&g_cs_handle);
      return 1;
    }
  }

  //
  char applet_dir[1024] = {0};
  get_dir_from_fullpath(filename, applet_dir, sizeof(applet_dir));
  zm_fs_set_data_dir(applet_dir);

  // 启动 applet（init → 绘制 → 停止）
  zm_emu_start_applet();

  // 调试 & 自测（已拆至 test/test_diag.c）//diag 的意思是诊断
  // zm_diag_dump_buttons(g_uc);
  // // zm_diag_audio_test(g_uc);
  // zm_diag_auto_click();
  // zm_diag_run_event_loop();

  // 释放资源
  zm_audio_shutdown();
  zm_display_shutdown();
  zm_fs_shutdown();
  cs_close(&g_cs_handle);
  uc_close(g_uc);

  return 0;
}
// 测试主函数
//
#include "./log/log.h"
//
#include "./emu.h"
#include "./event.h"
#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/fs/zm_fs.h"
#include "./zmaee/gfx/zm_gfx.h"
#include "./zmaee/runtime/zm_runtime.h"
//
#include <SDL2/SDL.h>
#include <capstone/capstone.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
  // char *filename = "/home/apollo/文档/古时游戏/zm_emu/applet/00000102/"
  //                  "00000102.app"; // 测试文件1（向后兼容验证）
  char *filename = "/home/apollo/文档/古时游戏/zm_emu/applet/00000405/"
                   "00000405.app"; // 测试文件2（号码归属）
  // /home/apollo/文档/古时游戏/zmaee_emu/zemee/0000050c/0000050c.app
  // char *filename = "/home/apollo/文档/古时游戏/zmaee_emu/zemee/0000050c/"
  //                  "0000050c.app"; // 测试文件3

  // 先解析 applet 头（不依赖 uc），以获取屏幕尺寸
  {
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
      log_error("fopen failed");
      cs_close(&cs_handle);
      return 1;
    }
    fseek(fp, 0, SEEK_END);
    long applet_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    log_info("文件大小: %ld\n", applet_size);

    parse_app_header(fp, &header);
    print_header(&header);
    fclose(fp);
  }

  // 初始化 SDL2 渲染与音频
  zm_rt_set_screen_size(header.ScreenW, header.ScreenH);
  if (zm_gfx_init(header.ScreenW, header.ScreenH) != 0) {
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
      return 1;
    }
    log_info("unicorn engine initialized");
  }

  // 内存映射
  if (zm_emu_map_memory(uc) != 0) {
    uc_close(uc);
    cs_close(&cs_handle);
    return 1;
  }

  // 构建虚表
  if (zm_emu_build_vtables(uc) != 0) {
    uc_close(uc);
    cs_close(&cs_handle);
    return 1;
  }

  // 注册钩子
  uc_hook hook_code_handle;
  uc_hook hook_unmapped_mem_handle;
  uc_hook hook_shim_mem_handle;
  if (zm_emu_add_hooks(uc, &hook_code_handle, &hook_unmapped_mem_handle,
                       &hook_shim_mem_handle) != 0) {
    uc_close(uc);
    cs_close(&cs_handle);
    return 1;
  }

  // 载入 blob 数据到客户机内存
  {
    long applet_size;
    if (zm_emu_load_blob(uc, filename, &applet_size) != 0) {
      uc_close(uc);
      cs_close(&cs_handle);
      return 1;
    }
  }

  // 载入 .zmr 资源
  zm_emu_load_zmr_if_exists(uc, filename);

  // 登记多文件 fs 表（00000405.app：config.b / zmsys006.dll / 图标等）
  // 从 filename 取目录部分传给 zm_fs_register_default
  {
    char applet_dir[1024];
    strncpy(applet_dir, filename, sizeof(applet_dir) - 1);
    applet_dir[sizeof(applet_dir) - 1] = '\0';
    char *slash = strrchr(applet_dir, '/');
    if (slash)
      *slash = '\0';
    zm_fs_register_default(applet_dir);
    // 登记 applet 自身（applet 可能通过 sprintf("%s%08x.app") 打开）
    zm_fs_register_hostfile(filename, "00000405.app");
  }

  // 启动 applet（init → 绘制 → 停止）
  zm_emu_start_applet(uc);

  // 调试：ZM_DUMP_BUTTONS=1 时打印 applet 在 init 中计算出的 25 个按钮矩形
  {
    const char *db = getenv("ZM_DUMP_BUTTONS");
    if (db && *db && g_instance) {
      for (uint32_t i = 0; i < 25; i++) {
        uint32_t off = g_instance + 124 + i * 16;
        uint32_t r[4];
        if (uc_mem_read(uc, off, r, 16) == UC_ERR_OK)
          log_info("按钮[%2u] rect=(%u,%u,%u,%u) center=(%u,%u)", i, r[0], r[1],
                   r[2], r[3], r[0] + r[2] / 2, r[1] + r[3] / 2);
      }
    }
  }

  // 音频自测
  {
    const char *atest = getenv("ZM_AUDIO_TEST");
    if (atest && *atest) {
      uint32_t count = zm_fs_get_resource_count();
      uint32_t idx = 0;
      if (strcmp(atest, "random") == 0) {
        srand((unsigned)time(NULL));
        idx = count ? (uint32_t)(rand() % count) : 0;
      } else {
        idx = (uint32_t)strtoul(atest, NULL, 0);
      }
      uint32_t rsize = 0;
      const uint8_t *rdata = zm_fs_get_resource(idx, &rsize);
      if (rdata && rsize && rsize <= ZMR_SIZE) {
        uc_mem_write(uc, ZMR_BASE, rdata, rsize);
        log_info("音频自测：播放资源 %u/%u  size=%u", idx, count, rsize);
        zm_ap_play(uc, ZMR_BASE, rsize);
      } else {
        log_warn("音频自测：资源 %u 不可用 (count=%u)", idx, count);
      }
    }
  }

  // 自动点击测试
  {
    const char *ac = getenv("ZM_AUTO_CLICK");
    if (ac && *ac) {
      unsigned ax = 0, ay = 0;
      if (sscanf(ac, "%u,%u", &ax, &ay) == 2) {
        log_info("自动点击测试: (%u,%u)", ax, ay);
        on_touch_click((uint32_t)ax, (uint32_t)ay);
      }
    }
  }

  // 事件循环
  {
    uint32_t hold_ms = 0;
    const char *env = getenv("ZM_GFX_HOLD_MS");
    if (env && *env)
      hold_ms = (uint32_t)strtoul(env, NULL, 0);
    zm_gfx_event_loop(on_touch_click, hold_ms);
  }

  // 释放资源
  zm_audio_shutdown();
  zm_gfx_shutdown();
  zm_fs_shutdown();
  cs_close(&cs_handle);
  uc_close(uc);

  return 0;
}
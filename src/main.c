// 测试主函数
//
/* glibc 在严格 -std=c23（xmake 的 set_languages("c23")）下默认隐藏 POSIX 符号，
 * 而"命令行参数覆盖同名环境变量"用到了 setenv（POSIX-2001）。
 * 特性宏必须在**任何**系统头之前定义，所以放在文件最顶上；只影响本文件。 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "./log/log.h"
//
#include "./emu.h"
#include "./test/test_diag.h"
#include "./test/zm_stat.h" /* zm_stat_init/dump：槽位与点击统计（ZM_STAT=1） */
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

/* ============================================================================
 *                        命令行参数 与 环境变量
 *
 * 优先级：**命令行 > 环境变量 > 内置默认**。
 *
 * 实现方式：argv 解析出来的值统一 setenv 成同名 ZM_* 变量。项目里已有 46 个
 * ZM_* 开关散在 12 个文件（zm_display.c / trap.c / emu.c …），绝大多数是
 * "用的时候才 getenv"——要把它们全改成命令行参数，要么引入全局 config 结构体、
 * 要么逐层透传，对**临时调试开关**并不划算。走 setenv 这层中转，
 * "命令行覆盖环境变量"就是免费的，其余模块一行都不用动。
 *
 * 分工：
 *   - 命令行：一等公民参数（跑哪个 applet、屏幕尺寸、日志级别）+ --help；
 *   - 环境变量：继续承载各种调试/诊断开关（与 SDL_VIDEODRIVER 同款惯例，
 *     `ZM_MW=… ZM_PC=… ./zm_emu` 组合起来最省事）。
 * ========================================================================== */

/* 常用分辨率预设。序号即 --screen / ZM_SCREEN 可用的序号；
 * 名字本身就是"宽x高"，所以直接写 320x480 也能选中同一档。 */
static const struct {
  const char *name;
  int w, h;
} k_screen_presets[] = {
    {"128x128", 128, 128}, {"128x160", 128, 160}, {"176x220", 176, 220},
    {"208x208", 208, 208}, {"240x320", 240, 320}, /* <- 默认档 */
    {"240x400", 240, 400}, {"320x240", 320, 240}, {"320x480", 320, 480},
    {"480x640", 480, 640}, {"480x800", 480, 800}, {"640x960", 640, 960},
};
#define ZM_SCREEN_PRESET_N \
  ((int)(sizeof(k_screen_presets) / sizeof(k_screen_presets[0])))
#define ZM_SCREEN_DEFAULT_IDX 4 /* 240x320 */

static void zm_print_screen_presets(void) {
  for (int i = 0; i < ZM_SCREEN_PRESET_N; i++)
    printf("  %2d = %-8s%s\n", i, k_screen_presets[i].name,
           i == ZM_SCREEN_DEFAULT_IDX ? "<- 默认" : "");
}

static void zm_print_help(const char *argv0) {
  const char *prog = argv0 ? strrchr(argv0, '/') : NULL;
  prog = prog ? prog + 1 : (argv0 ? argv0 : "zm_emu");

  printf("zm_emu —— zmaee applet 模拟器\n\n");
  printf("用法: %s [applet] [选项]\n\n", prog);
  printf("  applet                要运行的 .app：目录短名（如 000007ca）或完整路径；\n");
  printf("                        等价于 ZM_APPLET。都不给则用内置默认 applet。\n\n");
  printf("选项（给出的项会覆盖同名环境变量）:\n");
  printf("  --screen WxH|序号     屏幕尺寸；须落在该 applet 声明的支持区间内，\n");
  printf("                        越界自动夹到最近边界（序号见 --list-screens）\n");
  printf("  --log LEVEL           off|error|warn|info|debug（不设 = 全开）\n");
  printf("  --click \"x,y;x,y;...\"  无头自动按序点击，每 12 帧一个，用于脚本化复现\n");
  printf("  --mw lo,hi            监视该内存区间的写入（谁写的/写什么/PC/LR）\n");
  printf("  --pc lo[,hi]          PC 观察点：命中时打印 PC/LR/R0-R7\n");
  printf("  --pc2 lo[,hi]         第二个 PC 观察点\n");
  printf("  --trace               打印自定义 trap 的追踪（= ZM_SHOW_TRACE=1）\n");
  printf("  --disasm              逐条指令反汇编日志（= ZM_DISASM=1，量很大）\n");
  printf("  --screenshot 前缀      帧缓冲存 PNG；配合 --shot-every / --shot-max\n");
  printf("  --shot-every N        每 N 帧存一张（默认 1）\n");
  printf("  --shot-max N          最多存 N 张（默认 60）\n");
  printf("  --list-screens        列出分辨率预设后退出\n");
  printf("  -h, --help            显示本帮助后退出\n\n");
  printf("常用环境变量（多数调试开关只提供环境变量形式）:\n");
  printf("  ZM_APPLET ZM_SCREEN ZM_LOG ZM_CLICK ZM_MW ZM_MW_MAX ZM_PC ZM_PC2\n");
  printf("  ZM_SHOW_TRACE ZM_DISASM ZM_STEP ZM_STAT ZM_ULIBC_HEAP ZM_AUTO_RESUME\n");
  printf("  ZM_SCREENSHOT ZM_SHOT_EVERY ZM_SHOT_MAX ZM_FONT ZM_FONT_SIZE\n");
  printf("  ZM_DUMP_BUTTONS ZM_DUMP_LAYER ZM_GFX_HOLD_MS ZM_LOG_UCS2 ...\n");
  printf("  （全部开关：grep -rho 'getenv(\"ZM_[A-Z_0-9]*\")' src/）\n\n");
  printf("例:\n");
  printf("  %s 00000462 --screen 320x480\n", prog);
  printf("  %s applet/000007ca/000007ca.app --log info --click \"37,232;202,266\"\n",
         prog);
}

/* argv 覆盖同名环境变量（CLI > env）。Windows 没有 setenv，用 _putenv_s。 */
static void zm_set_env(const char *key, const char *val) {
#ifdef _WIN32
  _putenv_s(key, val);
#else
  setenv(key, val, 1);
#endif
}

/* 取选项值：支持 "--opt value" 与 "--opt=value"（后者由 eqval 传入） */
static const char *zm_arg_val(int argc, char **argv, int *i,
                              const char *eqval) {
  if (eqval)
    return eqval;
  if (*i + 1 < argc)
    return argv[++(*i)];
  return NULL;
}

/* 解析命令行。--help/--list-screens 直接打印后退出；其余选项 setenv 成
 * 同名 ZM_* 变量，交给后面原有代码读取。 */
static void zm_parse_args(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    char nb[64];
    const char *name = arg;
    const char *eqval = NULL;
    if (arg[0] == '-' && arg[1] == '-') { /* 拆 --opt=value */
      const char *eq = strchr(arg, '=');
      if (eq) {
        size_t n = (size_t)(eq - arg);
        if (n >= sizeof(nb))
          n = sizeof(nb) - 1;
        memcpy(nb, arg, n);
        nb[n] = '\0';
        name = nb;
        eqval = eq + 1;
      }
    }

    if (!strcmp(name, "-h") || !strcmp(name, "--help")) {
      zm_print_help(argv[0]);
      exit(0);
    } else if (!strcmp(name, "--list-screens")) {
      printf("分辨率预设（--screen / ZM_SCREEN 可用序号或名字）:\n");
      zm_print_screen_presets();
      exit(0);
    } else if (!strcmp(name, "--screen")) {
      const char *v = zm_arg_val(argc, argv, &i, eqval);
      if (!v) {
        fprintf(stderr, "错误: --screen 缺少参数（WxH 或序号）\n");
        exit(1);
      }
      zm_set_env("ZM_SCREEN", v);
    } else if (!strcmp(name, "--log")) {
      const char *v = zm_arg_val(argc, argv, &i, eqval);
      if (!v) {
        fprintf(stderr, "错误: --log 缺少参数（off/error/warn/info/debug）\n");
        exit(1);
      }
      zm_set_env("ZM_LOG", v);
    } else if (!strcmp(name, "--click")) {
      const char *v = zm_arg_val(argc, argv, &i, eqval);
      if (!v) {
        fprintf(stderr, "错误: --click 缺少参数（\"x,y;x,y\"）\n");
        exit(1);
      }
      zm_set_env("ZM_CLICK", v);
    } else if (!strcmp(name, "--mw")) {
      const char *v = zm_arg_val(argc, argv, &i, eqval);
      if (!v) {
        fprintf(stderr, "错误: --mw 缺少参数（lo,hi）\n");
        exit(1);
      }
      zm_set_env("ZM_MW", v);
    } else if (!strcmp(name, "--pc")) {
      const char *v = zm_arg_val(argc, argv, &i, eqval);
      if (!v) {
        fprintf(stderr, "错误: --pc 缺少参数（lo[,hi]）\n");
        exit(1);
      }
      zm_set_env("ZM_PC", v);
    } else if (!strcmp(name, "--pc2")) {
      const char *v = zm_arg_val(argc, argv, &i, eqval);
      if (!v) {
        fprintf(stderr, "错误: --pc2 缺少参数（lo[,hi]）\n");
        exit(1);
      }
      zm_set_env("ZM_PC2", v);
    } else if (!strcmp(name, "--trace")) {
      zm_set_env("ZM_SHOW_TRACE", "1");
    } else if (!strcmp(name, "--disasm")) {
      zm_set_env("ZM_DISASM", "1");
    } else if (!strcmp(name, "--screenshot")) {
      const char *v = zm_arg_val(argc, argv, &i, eqval);
      if (!v) {
        fprintf(stderr, "错误: --screenshot 缺少参数（路径前缀）\n");
        exit(1);
      }
      zm_set_env("ZM_SCREENSHOT", v);
    } else if (!strcmp(name, "--shot-every")) {
      const char *v = zm_arg_val(argc, argv, &i, eqval);
      if (!v) {
        fprintf(stderr, "错误: --shot-every 缺少参数\n");
        exit(1);
      }
      zm_set_env("ZM_SHOT_EVERY", v);
    } else if (!strcmp(name, "--shot-max")) {
      const char *v = zm_arg_val(argc, argv, &i, eqval);
      if (!v) {
        fprintf(stderr, "错误: --shot-max 缺少参数\n");
        exit(1);
      }
      zm_set_env("ZM_SHOT_MAX", v);
    } else if (arg[0] == '-' && arg[1]) {
      fprintf(stderr, "错误: 未知选项 %s（用 --help 看用法）\n", arg);
      exit(1);
    } else {
      /* 位置参数 = 要跑的 applet（等价 ZM_APPLET，但命令行优先） */
      zm_set_env("ZM_APPLET", arg);
    }
  }
}

int main(int argc, char **argv) {
  /* 先解析命令行：--help / --list-screens 在这里就退出了；其余选项 setenv 成
   * ZM_* 变量，下面的代码照旧用 getenv 读 —— 于是 "命令行 > 环境变量 > 默认"
   * 自然成立（--log 也能被紧接着的日志开关读到）。 */
  zm_parse_args(argc, argv);

  /* ---------------- 日志开关 ----------------
   * ZM_LOG=off   关闭全部日志（屏幕 + log.txt）—— 肉眼验证画面时用这个
   * ZM_LOG=error 只看错误   ZM_LOG=warn / info / debug 依次放宽
   * 不设该变量 = **info**（默认）。
   *
   * 【默认为什么不是全开】曾经默认是 LOG_TRACE（全开），后果是：debug 级日志里
   * 有"每次 shim 内存访问一行""每次外部槽调用一行"这类**逐事件**打印，一个忙等
   * 自旋的 applet 每秒能触发几万次 —— 终端被刷屏刷死、模拟器被同步 I/O 拖慢，
   * 看起来像"卡住"（实测踩过：1 毫秒十几行、连续不断）。想要这些细节时显式设
   * ZM_LOG=debug（或先看 ZM_SHIM_TRACE=1 只开 shim 区观察）。
   *
   * 注意：全量日志本身也很重——每行都要同时格式化到屏幕和 log.txt 两次。 */
  int log_level = LOG_INFO;
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

  /* 统计探针（ZM_STAT=1）：统计各槽位调用次数与点击坐标，退出时打印 Top 榜 */
  zm_stat_init();

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
  char *filename = "applet/00000102/"
                   "00000102.app"; //

  // char *filename = "applet/00000001/"
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
                 "applet/%s/%s.app", sel, sel);
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

  /* 层/窗口/GetDeviceInfo 的统一屏幕尺寸。
   *
   * 【头部 0x174~0x183 的真实语义】是 applet **支持的最小/最大分辨率区间**
   * （163 个样本统计：47 个 min=max=240x320；另有固定 240x400 / 320x480 的；
   *   其余是自适应型，如 00000462 = 176x220 ~ 800x800）。
   * 以前把"最大"直接当屏幕尺寸，自适应型 applet 就跑到 800x800/900x900：
   *   - 画面按 800 宽摆 UI，菜单横向平铺；
   *   - 00000462 在 800 宽下会按 屏宽/240+2 去维护滚动槽位（实测 240→3 槽、
   *     480→4 槽、720/800→5 槽），超过它对象里的数组容量（4 槽，第 5 槽
   *     obj+0xCC 是子对象指针）就踩掉指针，点"开始游戏"即 blx 到垃圾地址崩。
   *
   * 【选尺寸规则】
   *   1) 默认 240x320（经典功能机），见 k_presets 的默认档；
   *   2) ZM_SCREEN 可覆盖，两种写法：宽x高（"320x480"）或预设序号（"7"），
   *      预设表见 k_presets（名字就是分辨率本身，所以宽x高形式天然可用）；
   *   3) 选中的尺寸一律**夹进该 applet 声明的支持区间** [min,max]（越界取最近
   *      边界并打日志）——这就是"可以自由调，但不小于它要的最小、不大于最大"；
   *   4) 最后再按编译期容量上限 LAYER_MAX_W/H 夹紧（静态数组按上限预留）。 */
  {
    int w = k_screen_presets[ZM_SCREEN_DEFAULT_IDX].w;
    int h = k_screen_presets[ZM_SCREEN_DEFAULT_IDX].h;
    const char *src = "默认";

    const char *scr = getenv("ZM_SCREEN");
    if (scr && *scr) {
      int ew = 0, eh = 0;
      char *end = NULL;
      long n = strtol(scr, &end, 10);
      if (sscanf(scr, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) {
        w = ew;
        h = eh;
        src = "ZM_SCREEN=宽x高";
      } else if (end && end != scr && *end == '\0' && n >= 0 &&
                 n < ZM_SCREEN_PRESET_N) {
        w = k_screen_presets[n].w;
        h = k_screen_presets[n].h;
        src = "ZM_SCREEN=序号";
      } else {
        log_warn("ZM_SCREEN 无法识别: %s（可用 宽x高，如 320x480；或预设序号）",
                 scr);
      }
    }

    /* 把预设表打进日志，方便照抄名字或序号 */
    {
      char buf[512];
      int p = 0;
      for (int i = 0; i < ZM_SCREEN_PRESET_N && p < (int)sizeof(buf) - 24; i++)
        p += snprintf(buf + p, sizeof(buf) - (size_t)p, "%s%d=%s",
                      i ? " " : "", i, k_screen_presets[i].name);
      log_info("分辨率预设: %s", buf);
    }

    /* 夹进 applet 声明的支持区间（真源 = .app 头部 min/max） */
    const int minW = (int)g_header.MinScreenWidth;
    const int maxW = (int)g_header.MaxScreenWidth;
    const int minH = (int)g_header.MinScreenHeight;
    const int maxH = (int)g_header.MaxScreenHeight;
    if (minW > 0 && maxW >= minW && (w < minW || w > maxW)) {
      int cw = (w < minW) ? minW : maxW;
      log_warn("屏宽 %d 超出该 applet 支持区间 %d~%d，夹到 %d", w, minW, maxW,
               cw);
      w = cw;
    }
    if (minH > 0 && maxH >= minH && (h < minH || h > maxH)) {
      int ch = (h < minH) ? minH : maxH;
      log_warn("屏高 %d 超出该 applet 支持区间 %d~%d，夹到 %d", h, minH, maxH,
               ch);
      h = ch;
    }

    /* 编译期容量上限（静态数组/region 尺寸按上限预留） */
    if (w > LAYER_MAX_W) {
      log_warn("屏宽 %d 超模拟器上限 %d，夹紧", w, LAYER_MAX_W);
      w = LAYER_MAX_W;
    }
    if (h > LAYER_MAX_H) {
      log_warn("屏高 %d 超模拟器上限 %d，夹紧", h, LAYER_MAX_H);
      h = LAYER_MAX_H;
    }
    if (w <= 0)
      w = 240;
    if (h <= 0)
      h = 320;

    g_layer_w = w;
    g_layer_h = h;
    log_info("屏幕尺寸: %dx%d（%s；该 applet 支持区间 %dx%d ~ %dx%d）",
             g_layer_w, g_layer_h, src, minW, minH, maxW, maxH);
  }

  // 初始化 SDL2 渲染与音频

  /* ---- 显示初始化：没有窗口就没有意义，直接退出（除非是**主动要**的无头）----
   *
   * 老行为是"打一条 warning 然后继续跑"，后果是极难查的现象：日志一切正常、
   * 程序也不退，但永远没有窗口（Windows 上尤其容易被当成"程序不显示"）。
   *
   * 判据有**两条**，缺一不可：
   *   ① zm_display_init() 失败（SDL_Init / TTF_Init / 建窗口 / 建渲染器 / 建纹理）；
   *   ② init 成功但 SDL **实际选中**的驱动是 dummy/offscreen —— SDL 在没有显示时
   *      会自己回退到这些无头驱动，此时"窗口"建得出来却根本不在屏幕上。
   *      实测：DISPLAY 指向不存在的 X 且未设任何无头变量 → 视频驱动=offscreen、
   *      init 返回 0，程序照常跑满整场而屏幕上什么都没有。
   *
   * 无头模式（SDL_VIDEODRIVER=dummy/offscreen 或 ZM_HEADLESS=1）是**刻意不要
   * 窗口**的，那种情况继续跑（日志/截图仍可用，批量回归就是靠它）。 */
  {
    int disp_rc = zm_display_init();
    bool want_window = !zm_display_headless();
    bool no_window = (disp_rc != 0) || (want_window && zm_display_driver_is_headless());
    if (no_window && want_window) {
      if (disp_rc != 0)
        log_error("zm_display_init 失败：直接退出（原因见上面那条 error）");
      else
        log_error("SDL 实际用的是无头视频驱动（见上面\"视频驱动=…\"那行）："
                  "屏幕上不会有窗口，直接退出");
      /* ZM_LOG=off 会把 error 也一并吞掉（run.sh play 用的就是 off），
       * 所以这条致命信息必须再直接写一次 stderr，否则会"静默退出"。 */
      fprintf(stderr,
              "zm_emu: 拿不到窗口（显示初始化失败或驱动降级为无头），直接退出。"
              "如确实要无头运行，请设 SDL_VIDEODRIVER=dummy 或 ZM_HEADLESS=1\n");
      zm_display_shutdown();
      cs_close(&g_cs_handle);
      fclose(fp);
      return 1;
    }
    if (no_window)
      log_warn("无头模式：%s，继续运行（不会有窗口，日志/截图可用）",
               disp_rc != 0 ? "显示初始化失败" : "视频驱动是 dummy/offscreen");
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
  zm_diag_dump_buttons(g_uc);
  // // zm_diag_audio_test(g_uc);
  // zm_diag_run_event_loop();

  /* 统计探针收尾：打印"哪个槽位被疯狂调用 / 哪块坐标被反复点" */
  zm_stat_dump();

  // 释放资源
  zm_audio_shutdown();
  zm_display_shutdown();
  zm_fs_shutdown();
  cs_close(&g_cs_handle);
  uc_close(g_uc);

  return 0;
}
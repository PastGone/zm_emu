/* zm_emu —— ZMAEE ARM32 applet 模拟器入口
 *
 * 用法：
 *   zm_emu [选项] <applet.app | applet 目录 | applet 编号>
 *
 * 例：
 *   zm_emu applet/00000102/00000102.app
 *   zm_emu 00000102                       # 自动在 applet/ 下查找
 *   zm_emu -H -n 6 -o out/00000102 00000102   # 无头批量测试
 */

#include "./log/log.h"
//
#include "./emu.h"
#include "./event.h"
#include "./test/test_diag.h"
#include "./test/test_lib.h"
#include "./test/test_parse.h"
#include "./tool/odds.h"
#include "./trap.h"
#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/fs/zm_fs.h"
#include "./zmaee/gfx/zm_gfx.h"
//
#include <SDL2/SDL.h>
#include <capstone/capstone.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unicorn/unicorn.h>

/* ---------------- 运行参数 ---------------- */
typedef struct {
  const char *target;   /* 用户给的 applet 路径 / 目录 / 编号 */
  const char *out_dir;  /* 截图与写沙箱目录 */
  const char *log_file; /* 日志文件；NULL 表示不写文件 */
  int strict;           /* 1 = 有未实现调用就以非 0 退出 */
  int selftest;         /* 1 = 只跑依赖库自检 */
  const char *parse_only; /* 非 NULL = 只解析该 .app 的文件头 */
  int log_level;
} Options;

static void usage(const char *argv0) {
  printf(
      "zm_emu —— ZMAEE ARM32 applet 模拟器\n"
      "\n"
      "用法: %s [选项] <applet.app | applet 目录 | applet 编号>\n"
      "\n"
      "选项:\n"
      "  -H, --headless        无窗口模式（SDL dummy 驱动），用于自动化测试\n"
      "  -n, --max-events N    最多派发 N 轮事件后正常结束（0=不限，默认 0）\n"
      "  -c, --clicks N        无头模式下注入 N 次合成点击（默认 0）\n"
      "  -t, --hold-ms N       有窗口模式下每轮事件循环最长等待毫秒（0=直到关窗）\n"
      "  -o, --out DIR         输出目录：截图 screen.bmp + 文件写沙箱\n"
      "  -l, --log-level LV    trace/debug/info/warn/error/fatal（默认 info）\n"
      "      --log FILE        同时把日志写入文件\n"
      "  -d, --disasm          逐指令反汇编（很慢，仅调试用）\n"
      "  -s, --strict          存在未实现的外部调用时以退出码 3 结束\n"
      "      --step            每次外部调用后等待回车（交互调试）\n"
      "      --selftest        只跑依赖库自检（unicorn/capstone/SDL/log）\n"
      "      --parse FILE      只解析并打印 .app 文件头\n"
      "  -h, --help            显示本帮助\n"
      "\n"
      "退出码: 0=成功  1=参数/初始化错误  2=模拟异常终止  3=严格模式下有未实现调用\n",
      argv0);
}

static int parse_log_level(const char *s) {
  if (!s)
    return LOG_INFO;
  if (!strcmp(s, "trace"))
    return LOG_TRACE;
  if (!strcmp(s, "debug"))
    return LOG_DEBUG;
  if (!strcmp(s, "info"))
    return LOG_INFO;
  if (!strcmp(s, "warn"))
    return LOG_WARN;
  if (!strcmp(s, "error"))
    return LOG_ERROR;
  if (!strcmp(s, "fatal"))
    return LOG_FATAL;
  return LOG_INFO;
}

static bool is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* 在目录中找到第一个 .app 文件 */
static bool find_app_in_dir(const char *dir, char *out, size_t cap) {
  DIR *d = opendir(dir);
  if (!d)
    return false;
  struct dirent *e;
  bool found = false;
  char best[1024] = {0};
  while ((e = readdir(d)) != NULL) {
    size_t n = strlen(e->d_name);
    if (n > 4 && strcmp(e->d_name + n - 4, ".app") == 0) {
      snprintf(best, sizeof(best), "%s/%s", dir, e->d_name);
      found = true;
      break;
    }
  }
  closedir(d);
  if (found)
    snprintf(out, cap, "%s", best);
  return found;
}

/**
 * 把用户输入解析成真实的 .app 路径。支持：
 *   1) 直接给 .app 文件
 *   2) 给一个目录（取里面第一个 .app）
 *   3) 只给编号 "00000102"（依次在 ./applet/、../applet/ 下查找）
 */
static bool resolve_applet(const char *input, char *out, size_t cap) {
  if (!input || !*input)
    return false;

  if (is_file(input)) {
    snprintf(out, cap, "%s", input);
    return true;
  }
  if (is_dir(input))
    return find_app_in_dir(input, out, cap);

  const char *roots[] = {"applet", "./applet", "../applet", "../../applet"};
  char cand[1024];
  for (size_t i = 0; i < sizeof(roots) / sizeof(*roots); i++) {
    snprintf(cand, sizeof(cand), "%s/%s/%s.app", roots[i], input, input);
    if (is_file(cand)) {
      snprintf(out, cap, "%s", cand);
      return true;
    }
    snprintf(cand, sizeof(cand), "%s/%s", roots[i], input);
    if (is_dir(cand))
      return find_app_in_dir(cand, out, cap);
  }
  return false;
}

static void mkdir_p(const char *path) {
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv) {
  Options opt = {0};
  opt.log_level = LOG_INFO;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
#define NEXT_ARG()                                                             \
  ((i + 1 < argc) ? argv[++i] : (fprintf(stderr, "选项 %s 缺少参数\n", a),     \
                                 exit(1), (const char *)NULL))

    if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
      usage(argv[0]);
      return 0;
    } else if (!strcmp(a, "-H") || !strcmp(a, "--headless")) {
      g_headless = 1;
    } else if (!strcmp(a, "-n") || !strcmp(a, "--max-events")) {
      g_max_events = (uint32_t)strtoul(NEXT_ARG(), NULL, 0);
    } else if (!strcmp(a, "-c") || !strcmp(a, "--clicks")) {
      g_auto_clicks = (uint32_t)strtoul(NEXT_ARG(), NULL, 0);
    } else if (!strcmp(a, "-t") || !strcmp(a, "--hold-ms")) {
      g_hold_ms = (uint32_t)strtoul(NEXT_ARG(), NULL, 0);
    } else if (!strcmp(a, "-o") || !strcmp(a, "--out")) {
      opt.out_dir = NEXT_ARG();
    } else if (!strcmp(a, "-l") || !strcmp(a, "--log-level")) {
      opt.log_level = parse_log_level(NEXT_ARG());
    } else if (!strcmp(a, "--log")) {
      opt.log_file = NEXT_ARG();
    } else if (!strcmp(a, "-d") || !strcmp(a, "--disasm")) {
      g_disasm = 1;
    } else if (!strcmp(a, "-s") || !strcmp(a, "--strict")) {
      opt.strict = 1;
    } else if (!strcmp(a, "--step")) {
      g_trap_pause = 1;
    } else if (!strcmp(a, "--selftest")) {
      opt.selftest = 1;
    } else if (!strcmp(a, "--parse")) {
      opt.parse_only = NEXT_ARG();
    } else if (a[0] == '-' && a[1] != '\0') {
      fprintf(stderr, "未知选项: %s（用 --help 查看用法）\n", a);
      return 1;
    } else {
      opt.target = a;
    }
#undef NEXT_ARG
  }

  /* 环境变量兼容旧用法 */
  if (getenv("ZM_STEP"))
    g_trap_pause = 1;
  if (getenv("ZM_DISASM"))
    g_disasm = 1;
  {
    const char *e = getenv("ZM_GFX_HOLD_MS");
    if (e && *e && g_hold_ms == 0)
      g_hold_ms = (uint32_t)strtoul(e, NULL, 0);
  }

  log_set_level(opt.log_level);
  FILE *logfp = NULL;
  if (opt.log_file) {
    logfp = fopen(opt.log_file, "w");
    if (logfp)
      log_add_fp(logfp, LOG_TRACE);
  }

  if (opt.selftest)
    return test_lib();
  if (opt.parse_only)
    return test_parse(opt.parse_only);

  if (!opt.target) {
    usage(argv[0]);
    return 1;
  }

  char app_path[2048];
  if (!resolve_applet(opt.target, app_path, sizeof(app_path))) {
    log_error("找不到 applet: %s", opt.target);
    return 1;
  }
  snprintf(g_app_pathname, sizeof(g_app_pathname), "%s", app_path);
  log_info("applet 文件: %s", app_path);

  /* ---- 解析文件头（拿到屏幕尺寸与应用名） ---- */
  FILE *fp = fopen(app_path, "rb");
  if (!fp) {
    log_error("打开失败: %s", app_path);
    return 1;
  }
  long applet_size = get_file_size(fp);
  log_info("文件大小: %ld 字节", applet_size);
  if (!parse_app_header(fp, &g_header)) {
    log_error("文件头解析失败（文件可能不是 .app）");
    fclose(fp);
    return 1;
  }
  print_header(&g_header);

  /* ---- 输出目录 / 写沙箱 ---- */
  if (opt.out_dir) {
    mkdir_p(opt.out_dir);
    char fsroot[1100];
    snprintf(fsroot, sizeof(fsroot), "%s/fsroot", opt.out_dir);
    mkdir_p(fsroot);
    zm_fs_set_write_dir(fsroot);
  }

  /* ---- SDL 渲染 / 音频 ---- */
  bool gfx_ok = (zm_gfx_init() == 0);
  if (!gfx_ok)
    log_warn("图形初始化失败，渲染不可用（继续运行）");
  if (zm_audio_init() != 0)
    log_warn("音频初始化失败，声音不可用（继续运行）");

  int rc = 0;
  uc_err err;

  if (cs_open(CS_ARCH_ARM, CS_MODE_ARM, &g_cs_handle) != CS_ERR_OK) {
    log_error("Capstone 初始化失败");
    fclose(fp);
    rc = 1;
    goto cleanup_sdl;
  }

  err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &g_uc);
  if (err != UC_ERR_OK) {
    log_error("uc_open 失败: %s", uc_strerror(err));
    fclose(fp);
    cs_close(&g_cs_handle);
    rc = 1;
    goto cleanup_sdl;
  }
  log_info("Unicorn ARM32 引擎已就绪");

  if (zm_emu_map_memory() != 0 || zm_emu_build_vtables() != 0 ||
      zm_emu_add_hooks() != 0) {
    fclose(fp);
    rc = 1;
    goto cleanup_uc;
  }

  if (zm_emu_load_blob(fp, &applet_size) != 0) {
    /* zm_emu_load_blob 内部已 fclose */
    rc = 1;
    goto cleanup_uc;
  }

  /* applet 所在目录作为资源读取根 */
  {
    char applet_dir[1024] = {0};
    get_dir_from_fullpath(app_path, applet_dir, sizeof(applet_dir));
    zm_fs_register_default(applet_dir);
  }

  /* ---- 跑起来 ---- */
  if (zm_emu_start_applet() != 0)
    rc = 2;

  /* ---- 收尾：截图 + 摘要 ---- */
  {
    uint32_t nonzero = 0, colors = 0;
    if (gfx_ok && zm_gfx_ready()) {
      zm_gfx_canvas_stats(&nonzero, &colors);
      if (opt.out_dir) {
        char shot[1100];
        snprintf(shot, sizeof(shot), "%s/screen.bmp", opt.out_dir);
        if (zm_gfx_save_bmp(shot) == 0)
          log_info("截图已保存: %s", shot);
      }
    }

    log_info("──────── 运行摘要 ────────");
    log_info("applet      : %s (%s)", get_filename_from_fullpath(app_path),
             g_header.AppName);
    log_info("屏幕        : %ux%u", g_header.ScreenW, g_header.ScreenH);
    log_info("事件轮数    : %u", g_event_rounds);
    log_info("打开文件数  : %u", zm_fs_open_success_count());
    log_info("画面像素    : 非黑 %u，颜色数 %u", nonzero, colors);
    log_info("未实现调用  : %u", g_unknown_traps);
    log_info("退出状态    : %s", rc == 0 ? "正常" : "异常");
    zm_trap_dump_unknown_stats();

    /* 机器可读摘要，供测试脚本 grep */
    printf("ZM_SUMMARY applet=%s events=%u files=%u pixels=%u colors=%u "
           "unknown=%u rc=%d\n",
           get_filename_from_fullpath(app_path), g_event_rounds,
           zm_fs_open_success_count(), nonzero, colors, g_unknown_traps, rc);
    fflush(stdout);
  }

  if (rc == 0 && opt.strict && g_unknown_traps > 0)
    rc = 3;

cleanup_uc:
  cs_close(&g_cs_handle);
  if (g_uc)
    uc_close(g_uc);
cleanup_sdl:
  zm_audio_shutdown();
  zm_gfx_shutdown();
  zm_fs_shutdown();
  if (logfp)
    fclose(logfp);
  return rc;
}

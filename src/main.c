// 测试主函数
#define TEST
#ifdef TEST
#include "./test/test_lib.h"
#include "./test/test_parse.h"
#endif
//
#include "./log/log.h" //第三方实现的日志库
#include "./tool/paser_info.h"
//
#include <SDL2/SDL.h>
#include <capstone/capstone.h>
#include <unicorn/unicorn.h>

struct zm_rom_info {};

struct zm_rom {
  struct zm_rom_info *info;
  FILE *file;
};

int main() {
  log_info("hello world!");
  FILE *fp =
      fopen("/home/apollo/文档/古时游戏/zmaee_emu/applet/00000102/00000102.app",
            "rb");
  if (fp == NULL) {
    log_error("fopen failed");
    return 1;
  }

  AppHeader header;
  parse_app_header(fp, &header);
  print_header(&header);

#ifdef TEST
  // test_parse();
  test_lib();
#endif

  return 0;
}

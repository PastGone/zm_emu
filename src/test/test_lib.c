
#include "../log/log.h"
#include <SDL2/SDL.h>
#include <capstone/capstone.h>
#include <stdio.h>
#include <unicorn/unicorn.h>

// 待模拟的 x86 32位机器码: INC ecx; DEC edx
#define X86_CODE32 "\x41\x4a"
// 模拟执行的内存起始地址
#define ADDRESS 0x1000000

int uni_main() {
  uc_engine *uc; // Unicorn 引擎句柄
  uc_err err;
  int r_ecx = 0x1234; // ECX 初始值
  int r_edx = 0x7890; // EDX 初始值

  printf("开始模拟 x86 代码...\n");

  // 1. 初始化 x86 32位模拟器
  err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
  if (err != UC_ERR_OK) {
    printf("uc_open() 失败: %u\n", err);
    return -1;
  }

  // 2. 映射 2MB 内存
  uc_mem_map(uc, ADDRESS, 2 * 1024 * 1024, UC_PROT_ALL);

  // 3. 将机器码写入内存
  if (uc_mem_write(uc, ADDRESS, X86_CODE32, sizeof(X86_CODE32) - 1)) {
    printf("写入模拟代码到内存失败!\n");
    return -1;
  }

  // 4. 设置寄存器初始值
  uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx);
  uc_reg_write(uc, UC_X86_REG_EDX, &r_edx);

  // 5. 开始模拟执行
  err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(X86_CODE32) - 1, 0, 0);
  if (err) {
    printf("uc_emu_start() 失败: %u: %s\n", err, uc_strerror(err));
  }

  // 6. 读取并打印执行后的寄存器值
  uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx);
  uc_reg_read(uc, UC_X86_REG_EDX, &r_edx);
  printf("模拟完成!\n");
  printf(">>> ECX = 0x%x (应为 0x1235)\n", r_ecx);
  printf(">>> EDX = 0x%x (应为 0x788f)\n", r_edx);

  // 7. 清理资源
  uc_close(uc);
  return 0;
}

int sdl2_main() {

  // 1. 初始化视频子系统
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL 初始化失败: %s\n", SDL_GetError());
    return 1;
  }

  // 2. 创建窗口 (800x600)
  SDL_Window *window =
      SDL_CreateWindow("Hello SDL", SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_SHOWN);
  if (!window) {
    printf("窗口创建失败: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  // 3. 创建渲染器 (负责绘图)
  SDL_Renderer *renderer =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) {
    printf("渲染器创建失败: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // 4. 主循环标志
  int running = 1;
  SDL_Event event;

  while (running) {
    // 处理事件（比如点击关闭按钮）
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = 0;
      }
    }

    // 清屏为蓝色 (R=0, G=0, B=255, A=255)
    SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
    SDL_RenderClear(renderer);

    // 此处可以绘制图形... (留白)

    // 更新屏幕显示
    SDL_RenderPresent(renderer);

    // 简单的帧率控制（延时16ms约60帧）
    SDL_Delay(16);
  }

  // 5. 释放资源
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

int log_main() {
  log_set_level(LOG_TRACE);

  log_trace("hello world!");
  log_debug("hello world!");
  log_info("hello world!");
  log_warn("hello world!");
  log_error("hello world!");
  log_fatal("hello world!");

  return 0;
}

int capstone_main() {
  // 要反汇编的 x86-64 机器码（对应 "mov eax, 0x12345678" 和 "ret"）
  unsigned char code[] = {0xB8, 0x78, 0x56, 0x34, 0x12, 0xC3};
  size_t code_size = sizeof(code);
  uint64_t address = 0x1000; // 虚拟基址

  csh handle;
  cs_insn *insn;
  size_t count;

  // 初始化 Capstone，使用 x86-64 架构（CS_ARCH_X86，CS_MODE_64）
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) {
    fprintf(stderr, "Failed to open Capstone\n");
    return 1;
  }

  // 反汇编，输出到 insn 数组
  count = cs_disasm(handle, code, code_size, address, 0, &insn);
  if (count > 0) {
    for (size_t i = 0; i < count; i++) {
      printf("0x%" PRIx64 ":\t%s\t\t%s\n", insn[i].address, insn[i].mnemonic,
             insn[i].op_str);
    }
    // 释放动态分配的内存
    cs_free(insn, count);
  } else {
    fprintf(stderr, "Disassembly failed\n");
  }

  // 关闭 Capstone
  cs_close(&handle);
  return 0;
}

int test_lib() {
  printf("hello world!\n");
  log_main();
  uni_main();
  capstone_main();
  int ret = sdl2_main();
  if (ret != 0) {
    return ret;
  }

  return 0;
}

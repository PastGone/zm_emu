// 测试主函数
#define TEST
#ifdef TEST
#include "./test/test_lib.h"
#include "./test/test_parse.h"
#endif
//
#include "./log/log.h" //第三方实现的日志库
#include "./tool/paser_info.h"
#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/core/zm_mem.h"
#include "./zmaee/core/zm_str.h"
#include "./zmaee/fs/zm_fs.h"
#include "./zmaee/gfx/zm_gfx.h"
#include "./zmaee/runtime/zm_runtime.h"
//
#include <SDL2/SDL.h>
#include <capstone/capstone.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unicorn/arm.h>
#include <unicorn/unicorn.h>

// 映射内存
#define ONE_MB (0x100000) // 这里说的1MB是1MiB
//
#define BLOB_BASE (0x80000)    // blob 基础地址 512KB处
#define BLOB_SIZE (1 * ONE_MB) // blob 大小 1MB

// 栈
#define STACK_BASE (BLOB_BASE + BLOB_SIZE)  // 栈基础地址 512KB处 + 1MB
#define STACK_SIZE (1 * ONE_MB)             // 栈大小 1MB
#define STACK_TOP (STACK_BASE + STACK_SIZE) // 栈顶部地址 512KB处 + 1MB
// 堆
#define HEAP_BASE (STACK_TOP + ONE_MB / 8) // 离初始栈128KB处
#define HEAP_SIZE (6 * ONE_MB)             // 堆大小 6MB
#define HEAP_END (HEAP_BASE + HEAP_SIZE)   //  + 6MB
// shim 蹦床——>虚表
#define SHIM_BASE (HEAP_END)
#define SHIM_SIZE (1 * ONE_MB)
// tramp 陷阱，调用外部。
#define TRAMP_BASE (SHIM_BASE + SHIM_SIZE)
#define TRAMP_SIZE (1 * ONE_MB)
// zmr
#define ZMR_BASE (TRAMP_BASE + TRAMP_SIZE)
#define ZMR_SIZE (1 * ONE_MB)
// 根槽偏移量 0x180
#define ROOT_SLOT_OFF 0x180

// ---------------------
// 划分shim虚表空间
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

// 外部函数，陷阱地址分配,tramp空间分配
#define TRAP(idx) (TRAMP_BASE + 4 * (idx))
uint32_t TR_root_queryRuntime = TRAP(0);
uint32_t TR_root_malloc = TRAP(1);
uint32_t TR_root_free = TRAP(2);
// 字符串操作
uint32_t TR_root_str_copy = TRAP(3);
uint32_t TR_root_sprintf = TRAP(4);
uint32_t TR_root_str_ctor = TRAP(5);
uint32_t TR_root_spec_lookup = TRAP(6);
uint32_t TR_root_str_find = TRAP(7);
//
uint32_t TR_rt_queryInterface = TRAP(8);
uint32_t TR_rt_getSystemInfo = TRAP(9);
//
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
uint32_t TR_audio_stop = TRAP(20);
uint32_t TR_ap_play = TRAP(21);
uint32_t TR_ap_stop = TRAP(22);
//
uint32_t TR_init_callback = TRAP(100); // 这个实际上不知道这个是我随便定的
uint32_t SIZE_SLOT = SHIM_BASE + 0x700;
uint32_t API_SLOT = SHIM_BASE + 0x710;

//
uc_engine *uc;
AppHeader header;

//
/* 堆指针 */
uint32_t heap_ptr = HEAP_BASE;

void handle_trap(uc_engine *uc, uint32_t trap_address, uint32_t r0, uint32_t r1,
                 uint32_t r2, uint32_t r3, uint32_t sp, uint32_t lr) {
  uint32_t ret = 0;
  // const char *log = NULL;
  // char buf[256];

  if (trap_address == TR_init_callback) {
    uint32_t size;
    if (uc_mem_read(uc, SIZE_SLOT, &size, 4) != UC_ERR_OK) {
      log_error("Failed to read size");
      return;
    }
    uint32_t handler;
    if (uc_mem_read(uc, API_SLOT + 8, &handler, 4) != UC_ERR_OK) {
      log_error("Failed to read handler");
      return;
    }

    log_info("  size=%d handler=0x%X\n", size, handler);
    uint32_t INSTANCE = host_malloc(&heap_ptr, size);
    // 将 Unicorn 模拟器（虚拟机）的虚拟内存中，从地址 INSTANCE 开始、长度为
    // size 的一块区域，全部填充为 0（清零）
    uint8_t *zero_buf = calloc(1, size);
    uc_mem_write(uc, INSTANCE, zero_buf, size);
    free(zero_buf);
    //

    // 与 main.txt.c 一致：实例名需带 ".app" 后缀，applet 初始化时会用
    // str_find 在名字里查找 '.'（分隔扩展名），缺省后缀会走错分支。
    char app_name[64];
    snprintf(app_name, sizeof(app_name), "%.*s.app",
             (int)sizeof(header.AppName), header.AppName);
    uc_mem_write(uc, INSTANCE + 4, app_name, strlen(app_name) + 1);
    log_info("AppName: %s\n", app_name);
    log_info("  instance=0x%X\n", INSTANCE);

    uint32_t stack_ptr = STACK_TOP;
    uc_reg_write(uc, UC_ARM_REG_SP, &stack_ptr);

    uc_reg_write(uc, UC_ARM_REG_R0, &INSTANCE);
    // uc_reg_write 第3个参数是“指向值的指针”，不能传 0(NULL)，
    // 否则 unicorn 内部会解引用 NULL 读取寄存器值 → 段错误。
    uint32_t zero = 0;
    uc_reg_write(uc, UC_ARM_REG_R1, &zero);
    uc_reg_write(uc, UC_ARM_REG_R2, &zero);
    uc_reg_write(uc, UC_ARM_REG_R3, &zero);
    // init 执行完毕后让其返回到 uc_emu_start 的结束地址（STACK_TOP），
    // 这样 init 一次绘制完成后即停止模拟。若把 LR 设成传入的 lr，由于
    // uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    // 入口处 LR=TR_init_callback、sub_A98 用 bx lr 跳入本陷阱时 lr 未变，
    // 会导致 init 结束后 bx lr 又跳回 TR_init_callback，重复执行 init+draw，
    // 并在第二轮因栈顶上方 0x2801b8 未映射触发 MEM unmapped 才停住。
    uint32_t end_addr = STACK_TOP;
    uc_reg_write(uc, UC_ARM_REG_LR, &end_addr);

    uc_reg_write(uc, UC_ARM_REG_PC, &handler);
    return;
  }
  // root
  else if (trap_address == TR_root_queryRuntime) { // 查询运行时
    ret = RUNTIME;
  } else if (trap_address == TR_root_malloc) { // 分配内存
    // NOTE: main.txt.c 用 r0 作为 size；这里也应该使用 r0，待运行时确认
    ret = host_malloc(&heap_ptr, r0);
  } else if (trap_address == TR_root_free) { // 释放内存
    ret = 0;
  } else if (trap_address == TR_root_str_copy) { // 字符串复制
    ret = zm_strcpy(uc, r0, r1, r2, r3);
  } else if (trap_address == TR_root_sprintf) { // 格式化输出
    ret = zm_sprintf(uc, r0, r1, r2);
  } else if (trap_address == TR_root_str_ctor) { // 字符串构造函数
    ret = zm_str_ctor(uc, r0, r1);
  } else if (trap_address == TR_root_spec_lookup) { // 查找规格
    ret = zm_spec_lookup(uc, r0);
  } else if (trap_address == TR_root_str_find) { // 查找字符串
    ret = zm_str_find(uc, r0, r1);
  }
  /* ---- runtime ---- */
  else if (trap_address == TR_rt_queryInterface) {
    ret = zm_rt_queryInterface(uc, r1, r2);
  } else if (trap_address == TR_rt_getSystemInfo) {
    ret = zm_rt_getSystemInfo(uc, r1);
  }
  /* ---- gfx ---- */
  else if (trap_address == TR_gfx_clear) {
    ret = zm_gfx_clear(uc, r1);
  } else if (trap_address == TR_gfx_fillRect) {
    ret = zm_gfx_fillRect(uc, r1);
  } else if (trap_address == TR_gfx_commit) {
    ret = zm_gfx_commit(uc);
  } else if (trap_address == TR_gfx_drawText) {
    ret = zm_gfx_drawText(uc, r1, r2, r3, sp);
  } else if (trap_address == TR_gfx_drawRect) {
    ret = zm_gfx_drawRect(uc, r1, r2, r3, sp);
  } else if (trap_address == TR_gfx_fillRect2) {
    ret = zm_gfx_fillRect2(uc, r1, r2, r3, sp);
  }
  /* ---- fs / file ---- */
  else if (trap_address == TR_fs_open) {
    ret = zm_fs_open(uc, r1);
  } else if (trap_address == TR_file_close) {
    ret = zm_file_close(uc);
  } else if (trap_address == TR_file_read) {
    ret = zm_file_read(uc, r1, r2);
  } else if (trap_address == TR_file_seek) {
    ret = zm_file_seek(uc, r1, r2);
  }
  /* ---- audio / ap ---- */
  else if (trap_address == TR_audio_stop) {
    ret = zm_audio_stop(uc);
  } else if (trap_address == TR_ap_play) {
    ret = zm_ap_play(uc, r2, r3);
  } else if (trap_address == TR_ap_stop) {
    ret = zm_ap_stop(uc);
  } else {
    log_error("非法的外部调用: 0x%08" PRIx32, trap_address);
    log_error("其陷阱号是: %d", (trap_address - TRAMP_BASE) / 4);
  }
  //
  uc_reg_write(uc, UC_ARM_REG_R0, &ret);
  uc_reg_write(uc, UC_ARM_REG_PC, &lr);
}

csh handle;
cs_insn *insn;
size_t count;
uint8_t code[16]; // 最大指令长度通常不超过 16 字节（ARM Thumb
                  // 可能更长，但安全起见可动态分配）
static void hook_code(uc_engine *uc, uint64_t address, uint32_t size,
                      void *user_data) {
  uint32_t pc = address;
  // 分析当前执行代码段
  {
    uc_mem_read(uc, pc, code, size); // 读取内存数据

    count = cs_disasm(handle, code, size, pc, 0, &insn);
    if (count > 0) {
      char line[256]; // 临时行缓冲区
      for (size_t i = 0; i < count; i++) {
        // 拼接地址
        int offset =
            snprintf(line, sizeof(line), "0x%08" PRIx64 ":  ", insn[i].address);

        // 拼接机器码（固定4字节宽度，便于对齐）
        for (int j = 0; j < 4; j++) {
          if (j < insn[i].size) {
            offset += snprintf(line + offset, sizeof(line) - offset, "%02x ",
                               insn[i].bytes[j]);
          } else {
            offset += snprintf(line + offset, sizeof(line) - offset, "   ");
          }
        }

        // 拼接指令（助记符和操作数）
        snprintf(line + offset, sizeof(line) - offset, "%-8s %s",
                 insn[i].mnemonic, insn[i].op_str);

        // 一次性输出整行到日志
        log_info("%s\n", line);
      }
      cs_free(insn, count); // 必须释放,动态分配的内存

    } else {
      fprintf(stderr, "Disassembly failed\n");
    }
  }

  if (pc >= TRAMP_BASE && pc < TRAMP_BASE + TRAMP_SIZE) {

    uint32_t r0, r1, r2, r3, sp, lr;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);

    log_info("trap pc: %d, r0: %d, r1: %d, r2: %d, r3: %d, sp: %d, lr: %d\n",
             pc, r0, r1, r2, r3, sp, lr);

    handle_trap(uc, pc, r0, r1, r2, r3, sp, lr);

    // 制造暂停：等待用户按回车
    log_info("按回车键继续...");
    scanf("%*c"); // 读取一个字符，但不保存（*表示赋值忽略）
  }
}

static bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address,
                              int size, int64_t value, void *user_data) {
  log_warn("  !! MEM unmapped @0x%" PRIx64 " size=%d\n", address, size);
  return false; // 不处理
}

int main() {
  log_info("hello world!");
  //
  // 初始化 Capstone，使用 ARM-64 架构（CS_ARCH_ARM，CS_MODE_64）
  if (cs_open(CS_ARCH_ARM, CS_MODE_ARM, &handle) != CS_ERR_OK) {
    fprintf(stderr, "Failed to open Capstone\n");
    return 1;
  }

  uc_err my_uc_err;

  // 打开 applet 文件
  char filename[1024];
  strcpy(filename,
         "/home/apollo/文档/古时游戏/zmaee_emu/applet/00000102/00000102.app");

  FILE *fp = fopen(filename, "rb");
  if (fp == NULL) {
    log_error("fopen failed");
    return 1;
  }

  // 获取文件大小
  long applet_size;
  {
    fseek(fp, 0, SEEK_END);  // 指针移到末尾
    applet_size = ftell(fp); // 获取偏移量
    fseek(fp, 0, SEEK_SET);  // 记得复位指针，否则读不到数据
    log_info("文件大小: %ld\n", applet_size);
  }

  // 解析 applet 头

  parse_app_header(fp, &header);
  print_header(&header);

  // 初始化 SDL2 渲染（窗口大小取自 AppHeader.ScreenW/ScreenH）与 SDL_mixer 音频
  // 同步把屏幕尺寸告知 runtime，使 applet 经 getSystemInfo 拿到的尺寸与窗口一致
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
      return 1;
    }
    log_info("unicorn engine initialized");
  }

  // 内存映射
  {
    my_uc_err = uc_mem_map(uc, BLOB_BASE, BLOB_SIZE, UC_PROT_ALL);
    my_uc_err = uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
    my_uc_err = uc_mem_map(uc, HEAP_BASE, HEAP_SIZE, UC_PROT_ALL);
    my_uc_err = uc_mem_map(uc, SHIM_BASE, SHIM_SIZE, UC_PROT_ALL);
    my_uc_err = uc_mem_map(uc, TRAMP_BASE, TRAMP_SIZE, UC_PROT_ALL);
    my_uc_err = uc_mem_map(uc, ZMR_BASE, ZMR_SIZE, UC_PROT_ALL);
    if (my_uc_err != UC_ERR_OK) {
      log_error("uc_mem_map failed, err: %d\n", my_uc_err);
      return 1;
    }
    log_info("内存映射完成");
  }

  // 构建垫片或者蹦床或者虚表虚表，指向陷阱地址
  {
    // root
    my_uc_err = uc_mem_write(uc, ROOT, &TR_root_queryRuntime, 4);
    my_uc_err = uc_mem_write(uc, ROOT + 0x008, &TR_root_malloc, 4);
    my_uc_err = uc_mem_write(uc, ROOT + 0x00C, &TR_root_free, 4);
    my_uc_err = uc_mem_write(uc, ROOT + 0x020, &TR_root_str_copy, 4);
    my_uc_err = uc_mem_write(uc, ROOT + 0x06C, &TR_root_sprintf, 4);
    my_uc_err = uc_mem_write(uc, ROOT + 0x088, &TR_root_str_ctor, 4);
    my_uc_err = uc_mem_write(uc, ROOT + 0x0A4, &TR_root_spec_lookup, 4);
    my_uc_err = uc_mem_write(uc, ROOT + 0x0A8, &TR_root_str_find, 4);

    // runtime
    my_uc_err = uc_mem_write(uc, RUNTIME, &RT_VT, 4);
    my_uc_err = uc_mem_write(uc, RT_VT + 0x08, &TR_rt_queryInterface, 4);
    my_uc_err = uc_mem_write(uc, RT_VT + 0x10, &TR_rt_getSystemInfo, 4);
    // gfx
    my_uc_err = uc_mem_write(uc, GFX, &GFX_VT, 4);
    my_uc_err = uc_mem_write(uc, GFX_VT + 0x20, &TR_gfx_clear, 4);
    my_uc_err = uc_mem_write(uc, GFX_VT + 0x2C, &TR_gfx_fillRect, 4);
    my_uc_err = uc_mem_write(uc, GFX_VT + 0x40, &TR_gfx_commit, 4);
    my_uc_err = uc_mem_write(uc, GFX_VT + 0x50, &TR_gfx_drawText, 4);
    my_uc_err = uc_mem_write(uc, GFX_VT + 0x6C, &TR_gfx_drawRect, 4);
    my_uc_err = uc_mem_write(uc, GFX_VT + 0x70, &TR_gfx_fillRect2, 4);

    // fs
    my_uc_err = uc_mem_write(uc, FS, &FS_VT, 4);
    my_uc_err = uc_mem_write(uc, FS_VT + 0x08, &TR_fs_open, 4);
    my_uc_err = uc_mem_write(uc, FILE1, &FILE_VT, 4);
    my_uc_err = uc_mem_write(uc, FILE_VT + 0x04, &TR_file_close, 4);
    my_uc_err = uc_mem_write(uc, FILE_VT + 0x08, &TR_file_read, 4);
    my_uc_err = uc_mem_write(uc, FILE_VT + 0x20, &TR_file_seek, 4);
    // audio
    my_uc_err = uc_mem_write(uc, AUDIO, &AUDIO_VT, 4);
    my_uc_err = uc_mem_write(uc, AUDIO_VT + 0x14, &TR_audio_stop, 4);
    my_uc_err = uc_mem_write(uc, AP, &AP_VT, 4);
    my_uc_err = uc_mem_write(uc, AP_VT + 0x10, &TR_ap_play, 4);
    my_uc_err = uc_mem_write(uc, AP_VT + 0x14, &TR_ap_stop, 4);
    if (my_uc_err != UC_ERR_OK) {
      log_error("uc_mem_write failed, err: %d\n", my_uc_err);
      return 1;
    }
    log_info("虚表构建完成");
  }
  // 设置钩子，拦截系统调用使它陷入陷阱函数嗯，就和前面相配合了
  uc_hook hook_code_handle;
  uc_hook hook_mem_handle;
  {
    log_info("添加钩子");
    my_uc_err =
        uc_hook_add(uc, &hook_code_handle, UC_HOOK_CODE, hook_code, NULL, 1, 0);
    if (my_uc_err != UC_ERR_OK) {
      log_error("uc_hook_add failed, err: %d\n", my_uc_err);
      return 1;
    }
    my_uc_err = uc_hook_add(uc, &hook_mem_handle, UC_HOOK_MEM_UNMAPPED,
                            hook_mem_unmapped, NULL, 1, 0);
    if (my_uc_err != UC_ERR_OK) {
      log_error("uc_hook_add failed, err: %d\n", my_uc_err);
      return 1;
    }
    log_info("钩子添加完成");
  }

  // 载入blob数据
  log_info("开始载入blob数据");
  {
    unsigned char *buf = malloc(applet_size);
    if (buf == NULL) {
      log_error("malloc failed");
      return 1;
    }

    size_t bytes_read = fread(buf, 1, applet_size, fp);
    if (bytes_read != applet_size) {
      log_error("读取文件失败，期望%zu字节，实际读取%zu", applet_size,
                bytes_read);
      free(buf);
      return 1;
    }

    uc_err err = uc_mem_write(uc, BLOB_BASE, buf, applet_size);
    if (err != UC_ERR_OK) {
      log_error("uc_mem_write failed, err: %d\n", err);
      return 1;
    }
    free(buf);
    log_info("blob数据载入完成");
    // 文件现在没用了准备释放文件
    fclose(fp);
    log_info("文件关闭完成");
  }

  // 载入 .zmr 资源（作为 file.read 的数据源）
  // 路径由 .app 路径把后缀替换为 .zmr 得到
  {
    char zmr_path[1024];
    strncpy(zmr_path, filename, sizeof(zmr_path) - 1);
    zmr_path[sizeof(zmr_path) - 1] = '\0';
    size_t plen = strlen(zmr_path);
    if (plen >= 4 && strcmp(zmr_path + plen - 4, ".app") == 0) {
      strcpy(zmr_path + plen - 4, ".zmr");
    } else {
      strncat(zmr_path, ".zmr", sizeof(zmr_path) - plen - 1);
    }
    if (!zm_fs_load_zmr(zmr_path)) {
      log_error("载入 .zmr 失败: %s", zmr_path);
      return 1;
    }
    log_info(".zmr 资源载入完成: %s", zmr_path);
  }
  // 设置初始的寄存器

  uc_reg_write(uc, UC_ARM_REG_LR, &TR_init_callback);
  uc_reg_write(uc, UC_ARM_REG_R0, &SIZE_SLOT);
  uc_reg_write(uc, UC_ARM_REG_R1, &API_SLOT);

  // 向  BLOB_BASE + ROOT_SLOT_OFF
  // 注入根槽地址,这一步实际上
  // 我不知道是什么发时候发生的但是需要的话现在就开始的时候就把它搞
  uc_mem_write(uc, BLOB_BASE + ROOT_SLOT_OFF, &ROOT, 4);

  // 启动 unicorn 引擎
#define APPLET_ENTRY_OFF 0x188
#define APPLET_ENTRY_POINT (BLOB_BASE + APPLET_ENTRY_OFF)

  log_info("启动unicorn engine...");
  uc_emu_start(uc, APPLET_ENTRY_POINT, STACK_TOP, 0, 0);
  log_info("unicorn engine启动完成");
#ifdef TEST
  // test_parse();
  // test_lib();
#endif

  // 音频自测：applet 的 init 不触发 ap.play（音频仅在触摸事件 sub_8B4
  // 中播放）， 故提供环境变量 ZM_AUDIO_TEST 让宿主直接按索引取 .zmr
  // 中的音频资源， 写入客户机内存后走真实 zm_ap_play 路径播放，以验证 SDL_mixer
  // 后端可用。
  //   用法：ZM_AUDIO_TEST=0        播放第 0 个资源
  //        ZM_AUDIO_TEST=3        播放第 3 个资源
  //        ZM_AUDIO_TEST=random   随机选一个
  // 未设置时跳过，不影响正常渲染。
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
        /* 把资源数据放到 ZMR_BASE 客户机区域，再走 ap.play 读取 */
        uc_mem_write(uc, ZMR_BASE, rdata, rsize);
        log_info("音频自测：播放资源 %u/%u  size=%u", idx, count, rsize);
        zm_ap_play(uc, ZMR_BASE, rsize);
      } else {
        log_warn("音频自测：资源 %u 不可用 (count=%u)", idx, count);
      }
    }
  }

  // applet 只跑一次 init、无事件循环，保持窗口显示最后一帧以便观察
  // （ZM_GFX_HOLD_MS 环境变量可控制停留毫秒数，默认 0=直到关闭窗口）
  {
    uint32_t hold_ms = 0;
    const char *env = getenv("ZM_GFX_HOLD_MS");
    if (env && *env)
      hold_ms = (uint32_t)strtoul(env, NULL, 0);
    zm_gfx_hold(hold_ms);
  }

  // 释放资源
  zm_audio_shutdown();
  zm_gfx_shutdown();
  zm_fs_shutdown();

  return 0;
}

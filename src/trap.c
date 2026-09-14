#include "./trap.h"
#include "./log/log.h"
#include "./test/zm_stat.h" /* zm_stat_trap：槽位调用次数统计（ZM_STAT=1） */
#include <inttypes.h> /* PRIx32，用于第 294 行格式化输出 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "./emu.h"
#include "./tool/odds.h"
#include "./tool/uc_helper.h" /* uc_read32/uc_write32（读栈上参数 a5） */
#include "./ulibc/ulibc.h" /* 客户机 libc：malloc/free/memcpy/sprintf/str* 等 */
#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/core/zm_mem.h"
#include "./zmaee/core/zm_root.h"
#include "./zmaee/core/zm_str.h"
#include "./zmaee/fs/zm_file_mgr.h"
#include "./zmaee/fs/zm_file.h"
#include "./zmaee/gfx/zm_display.h" /* ZMAEE IDisplay / IBitmap + SDL 渲染后端 */
#include "./zmaee/gfx/zm_image.h"   /* ZMAEE IImage（资源加载链） */
#include "./zmaee/runtime/shell/zm_shell.h"
#include "./zmaee/runtime/timer/zm_timer.h" /* IShell 定时器子系统 */
#include "event.h"
#include "zmaee/inc/zm_event_code.h"

/*
 * ==========================================================================
 * 客户机堆 / libc 的接线层
 * ==========================================================================
 * applet 通过 ROOT_TABLE_ADDR vtable 调用的这些槽位，语义上就是标准 C 库函数。
 * 这里把它们统一转接到 src/ulibc（跨地址空间的 libc 实现），
 * 由 ulibc 负责"客户机指针搬运"的全部脏活。
 *
 * 默认使用 ulibc 真实堆（g_ulibc_heap=1）：malloc 从空闲链表分配、
 * free 真正回收并与相邻空闲块合并，与原始固件的堆行为一致。
 * ZM_ULIBC_HEAP=0 回退到 bump 分配器，仅用于排查 applet 的
 * UAF / double-free（见 emu.h）。
 * ==========================================================================
 */

/** applet 的 malloc */
static uint32_t applet_malloc(uc_engine *uc, uint32_t size) {
  if (g_ulibc_heap)
    return u_malloc(uc, size);
  return host_malloc(&g_heap_ptr, size); /* 排查用：只增不减 */
}

/** applet 的 free */
static void applet_free(uc_engine *uc, uint32_t p) {
  if (g_ulibc_heap) {
    u_free(uc, p);
    return;
  }
  (void)uc;
  /* 排查模式：不回收，用于确认崩溃是否由内存回收引起 */
}

/** applet 的 calloc：分配并清零（清零在客户机侧完成，不开宿主临时缓冲） */
static uint32_t applet_calloc(uc_engine *uc, uint32_t n, uint32_t size) {
  uint32_t p = applet_malloc(uc, n * size);
  if (p)
    u_memset(uc, p, 0, n * size);
  return p;
}

uint32_t getArg(uc_engine *uc, uint32_t n) {
  uint64_t v64 = 0; // 关键：必须用 64 位容器
  uc_err err;

  // 1. 寄存器参数 R0 ~ R3
  if (n <= 3) {
    err = uc_reg_read(uc, UC_ARM_REG_R0 + n, &v64);
    if (err != UC_ERR_OK)
      return 0;
    return (uint32_t)v64;
  }

  // 2. 栈参数（前提：当前 PC 必须在函数第一条指令！）
  uint64_t sp = 0;
  err = uc_reg_read(uc, UC_ARM_REG_SP, &sp);
  if (err != UC_ERR_OK)
    return 0;
  if (sp == STACK_TOP) {
    log_info("sp is STACK_TOP");
    return 0;
  }
  // 第 n 个参数（n>=4）在入口栈顶的偏移 (n-4)*4
  uint64_t addr = sp + (n - 4) * 4;
  uint32_t result = 0;
  err = uc_mem_read(uc, addr, &result, 4);
  if (err != UC_ERR_OK)
    return 0;

  return result;
}
/**
 * 打印 Unicorn 模拟器中当前所有非零的 ARM32 核心寄存器
 * @param uc Unicorn 引擎句柄
 */
void print_non_zero_registers(uc_engine *uc) {
  // 定义需要遍历的寄存器结构
  typedef struct {
    int reg_id;       // Unicorn 定义的寄存器 ID (UC_ARM_REG_*)
    const char *name; // 寄存器名称（用于打印）
  } arm_reg_entry;

  // 针对 MT6250 (ARM7EJ-S) 的核心寄存器列表
  // 去掉了浮点/NEON/Cortex-M专有寄存器，因为MT6250通常不支持或不存在
  arm_reg_entry regs[] = {// 通用寄存器 R0 - R12
                          {UC_ARM_REG_R0, "R0"},
                          {UC_ARM_REG_R1, "R1"},
                          {UC_ARM_REG_R2, "R2"},
                          {UC_ARM_REG_R3, "R3"},
                          {UC_ARM_REG_R4, "R4"},
                          {UC_ARM_REG_R5, "R5"},
                          {UC_ARM_REG_R6, "R6"},
                          {UC_ARM_REG_R7, "R7"},
                          {UC_ARM_REG_R8, "R8"},
                          {UC_ARM_REG_R9, "R9"},
                          {UC_ARM_REG_R10, "R10"},
                          {UC_ARM_REG_R11, "R11"},
                          {UC_ARM_REG_R12, "R12"},
                          // 栈指针、链接寄存器、程序计数器
                          {UC_ARM_REG_SP, "SP"}, // R13
                          {UC_ARM_REG_LR, "LR"}, // R14
                          {UC_ARM_REG_PC, "PC"}, // R15
                                                 // 状态寄存器
                          {UC_ARM_REG_CPSR, "CPSR"},
                          {UC_ARM_REG_SPSR, "SPSR"}};

  int reg_count = sizeof(regs) / sizeof(regs[0]);
  uint32_t value = 0;
  uc_err err = UC_ERR_OK;
  int has_non_zero = 0; // 标记是否有非零寄存器

  printf("========== Non-zero Registers ==========\n");

  for (int i = 0; i < reg_count; i++) {
    // 读取寄存器值
    err = uc_reg_read(uc, regs[i].reg_id, &value);

    if (err == UC_ERR_OK) {
      if (value != 0) {
        printf("[+] %s: 0x%08X (%u)\n", regs[i].name, value, value);
        has_non_zero = 1;
      }
    } else {
      // 读取失败（例如在用户模式下读取 SPSR 会报错），静默跳过即可
      // 如果需要调试，可以取消下面注释：
      // printf("[!] Read %s failed (error: %d)\n", regs[i].name, err);
    }
  }

  if (!has_non_zero) {
    printf("(No non-zero registers found in current context.)\n");
  }
  printf("========================================\n");
}
void handle_trap(uc_engine *uc, uint32_t trap_address) {

  uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, sp = 0, lr = 0;
  /* 逐个检查寄存器读取结果，避免失败时使用未初始化值污染分发逻辑 */
  if (uc_reg_read(uc, UC_ARM_REG_R0, &r0) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_R1, &r1) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_R2, &r2) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_R3, &r3) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_SP, &sp) != UC_ERR_OK ||
      uc_reg_read(uc, UC_ARM_REG_LR, &lr) != UC_ERR_OK) {
    log_error("Failed to read core registers at trap 0x%08" PRIx32,
              trap_address);
    return;
  }
  /* 参数打印：仅在开启反汇编调试（ZM_DISASM/g_disasm）时输出。
   * 曾经是无条件 printf，每次 trap 都刷 10 行，既拖慢模拟又污染 stdout。 */
  if (g_disasm) {
    for (int i = 0; i < 10; i++) {
      uint32_t v = getArg(uc, i);
      log_debug("trap[0x%X] arg%d = 0x%08X", trap_address - TRAMP_BASE, i, v);
    }
  }
  // log_debug("trap addr: %d, r0: %d, r1: %d, r2: %d, r3: %d, sp: %d, lr:
  // %d\n",
  //           trap_address, r0, r1, r2, r3, sp, lr);

  /* 寄存器 dump 只在反汇编调试下输出（原来每次都刷，既慢又淹没有效日志） */
  if (g_disasm)
    print_non_zero_registers(uc);

  /* 统计探针（ZM_STAT=1）：在真正分发前记一笔，供"哪个槽位被疯狂调用"分析 */
  zm_stat_trap(trap_address - TRAMP_BASE);

  uint32_t ret = 0;
  switch (trap_address) {
  case TR_init_callback: {
    /*
     * applet 的"握手"入口。applet 启动代码在准备好之后会主动调用
     * ROOT_TABLE_ADDR+0x118C，含义是"固件，请按下面这份规格初始化我"。
     *
     * 规格通过**参数**给出：applet 的握手函数（如 00000506 的 sub_F0
     * 尾部 loc_8860）执行：
     *     *r0 = 需要的实例字节数      （STR R2,[R0]，R2=0x50）
     *     r1[0] = 0, r1[1] = 0, r1[2] = 事件处理回调地址
     *     BX LR                          （返回固件）
     * 因此固件必须传 r0 = &SIZE_SLOT、r1 = &API_SLOT，让它把这两个值
     * 写进约定的槽位；随后固件再读取它们分配实例、并用该回调发起
     * EV_CREATE。
     *
     * 旧实现把 r0 直接当成 instance 传进去（且跳 0x188），结果 applet
     * 把 0x50/回调写进了实例内存，API_SLOT+8 里残留旧值 0x1108C，
     * 后续事件派发全部走错分支 —— 表现为事件循环空转、零资源加载。
     */
    uint32_t a0 = SIZE_SLOT;
    uint32_t a1 = API_SLOT;
    uc_reg_write(uc, UC_ARM_REG_R0, &a0);
    uc_reg_write(uc, UC_ARM_REG_R1, &a1);

    /* 返回地址：握手函数 `BX LR` 回到这里，由宿主侧完成实例分配与
     * 事件循环驱动（见 TR_enter_event_loop 的状态机）。 */
    uint32_t callback_addr =
        g_registered_loop ? g_registered_loop : TR_enter_event_loop;
    uc_reg_write(uc, UC_ARM_REG_LR, &callback_addr);

    /* 入口 = payload 偏移 0x188（`b loc_8860` 的握手 trampoline）。 */
    uint32_t entry = APPLET_ENTRY_POINT;
    uc_reg_write(uc, UC_ARM_REG_PC, &entry);
    log_info("init 握手：r0=&SIZE_SLOT、r1=&API_SLOT，返回地址=0x%X",
             callback_addr);
    return;
  } break;
  case TR_register_event_loop:
    /* ROOT_TABLE_ADDR+0x1184：applet 注册它的事件循环入口（r0=handler 地址）。
     * 00000506 在 init 里先注册、再调 ROOT_TABLE_ADDR+0x118C()，随后返回；
     * 模拟器据此单独驱动事件循环。 */
    if (r0)
      g_registered_loop = r0;
    log_info("applet 注册事件循环入口: 0x%X", r0);
    ret = 0;
    break;

  case TR_abort:
    /* ROOT_TABLE_ADDR+0x14：applet 请求退出（实测 00000506 传 "aborted"）。 */
    if (g_disasm) {
      char msg[64];
      read_cstr(uc, r0, msg, sizeof(msg));
      log_info("applet 调用 abort(\"%s\")，停止模拟", msg);
    } else {
      log_info("applet 调用 abort，停止模拟");
    }
    uc_emu_stop(uc);
    return;

  case TR_enter_event_loop: {
    /* 宿主侧驱动主循环的状态机。每次 applet 的回调返回（`pop {pc}` 到
     * LR=TR_enter_event_loop）都会重新进入这里，按 stage 逐步推进：
     *
     *   stage 0：applet 刚跑完 init 握手（写了 SIZE_SLOT / API_SLOT），
     *            这里读出实例大小与事件回调 → 分配实例 → 发 EV_CREATE。
     *   stage 1：EV_CREATE 处理完 → 发 EV_RESUME（多家 applet 的
     *            界面创建/资源加载都挂在这个分支上）。
     *   stage 2：进入 SDL 事件循环等待用户交互。
     *
     * 每次派发事件后必须立刻 return，把控制权交回 unicorn 执行 handler；
     * 否则 handler 永远没机会跑（表现为事件循环空转、零绘制）。 */
    static int stage = 0;
    static int resume_enabled = 1;
    static int resume_inited = 0;

    /* applet 调过 IShell.CloseApplet（+0x24）：它的退出回调 EV_STOP 已跑完，收工 */
    if (zm_event_close_requested()) {
      log_info("applet 的退出回调已跑完（CloseApplet）→ 结束模拟");
      uc_emu_stop(uc);
      return;
    }

    if (!resume_inited) {
      const char *ar = getenv("ZM_AUTO_RESUME");
      resume_enabled = (!ar || ar[0] != '0');
      resume_inited = 1;
    }

    if (stage == 0) {
      uint32_t size = uc_read32(uc, SIZE_SLOT);
      uint32_t handler = uc_read32(uc, API_SLOT + 8);
      log_info("init 握手完成：size=%u handler=0x%X（API_SLOT=[%08X %08X %08X "
               "%08X]）",
               size, handler, uc_read32(uc, API_SLOT),
               uc_read32(uc, API_SLOT + 4), uc_read32(uc, API_SLOT + 8),
               uc_read32(uc, API_SLOT + 12));
      if (size == 0 || size > 0x40000 || handler == 0) {
        log_error("握手结果异常（size=%u handler=0x%X），无法继续", size,
                  handler);
        uc_emu_stop(uc);
        return;
      }

      uint32_t INSTANCE = applet_calloc(uc, 1, size);
      /* 把 applet 短名写入 instance+4：applet 用它在运行时构造
       * "<name>.zmr" 等资源文件名。写入前做边界校验。 */
      const char *filename = get_filename_from_fullpath(g_app_pathname);
      size_t fn_len = strlen(filename) + 1;
      if (4 + fn_len <= (size_t)size)
        uc_mem_write(uc, INSTANCE + 4, filename, fn_len);
      else
        log_warn("filename (len=%zu) 超出实例大小 %u，跳过写入", fn_len, size);

      g_instance = INSTANCE;
      g_handler = handler;
      log_info("实例已分配：instance=0x%X（%u 字节），handler=0x%X", INSTANCE,
               size, handler);

      stage = 1;
      log_info("派发 EV_CREATE(evt=0) -> handler=0x%X", g_handler);
      dispatch_applet_event(0 /* EV_CREATE */, 0, 0);
      return;
    }

    if (stage == 1) {
      stage = 2;
      if (resume_enabled) {
        log_info("派发 EV_RESUME(evt=3) -> handler=0x%X", g_handler);
        dispatch_applet_event(3, 0, 0);
        return;
      }
    }

    /* stage >= 2：进入 SDL 事件循环等待用户交互。
     * 返回 false → 模拟结束；返回 true → 已派发点击，继续执行 handler。
     * 必须 uc_emu_stop + return，否则会 fall-through 到 default
     * 误报"非法的外部调用"，且 PC 继续执行 TRAMP 区下一条指令导致越界。 */
    uint32_t hold_ms = 0;
    const char *env = getenv("ZM_GFX_HOLD_MS");
    if (env && *env)
      hold_ms = (uint32_t)strtoul(env, NULL, 0);
    /* 关窗/超时后的收尾：
     *   1) 已派发过 EV_STOP → 直接收工（**必须先判**，否则会再次进入事件循环，
     *      而 SDL_QUIT 已被消费，循环再也不返回 → 只能被 timeout -k 强杀）。
     *   2) 否则在 ZM_EXIT_EVENT=1 时先派发 EV_STOP(evt=1)，让 applet 自己跑完
     *      收尾（依据 00000506 入口 sub_87C8：evt=0 建屏、evt=1 释放屏幕；
     *      那条链路到 sub_9270：ISetting[+0x18](0) 关声音、pauseMusic、
     *      取消定时器、释放 UI 并存盘 data/farm）—— 也就是"applet 自己的
     *      退出回调"。handler 返回后回到本 trap，再走 1) 收工。
     * 默认关闭（ZM_EXIT_EVENT=1 开启）：部分 applet 的退出路径会踩到已释放
     * 的对象，开启前先确认该 applet 稳定。 */
    static int exit_dispatched = 0;
    if (exit_dispatched) {
      uc_emu_stop(uc);
      return;
    }
    if (!zm_display_event_loop(on_touch_click, hold_ms)) {
      const char *ex = getenv("ZM_EXIT_EVENT");
      if (ex && *ex && ex[0] != '0') {
        exit_dispatched = 1;
        log_info("派发 EV_STOP(evt=1) -> handler=0x%X（让 applet 自己收尾）",
                 g_handler);
        dispatch_applet_event(1, 0, 0);
        return;
      }
      uc_emu_stop(uc);
    }
    return;
  } //
  break;
    /* ---- ROOT_TABLE_ADDR ---- */
  case TR_root_getShell:
    ret = G_SHELL_ADDR;
    break;
  case TR_root_malloc: {
    /* 注意：真实签名是 malloc(size=r0, ctx=r1)，返回值直接是对象指针。
     * 之前误当成 (ctx, size) 来打日志，才让"0x823200 不属于堆"看起来矛盾。 */
    uint32_t a_size = r0;
    uint32_t a_ctx = r1;
    ret = applet_malloc(uc, r0);
    if (g_disasm)
      log_debug("malloc(size=%u r0, ctx=0x%X r1) lr=0x%X -> 0x%X", a_size, a_ctx,
                lr, ret);
    break;
  }
  case TR_root_free:
    log_debug("这里的话是 free(r0=%d)", r0);
    applet_free(uc, r0);
    ret = 0;
    break; /* free(r0=ptr) */
  case TR_root_utf8_to_ucs2:
    /* ROOT_TABLE_ADDR[0x20] = ZMAEE_Utf8_2_Ucs2(utf8_src=r0, 源字节数=r1,
     * ucs2_dst=r2, 目标字符容量=r3) → 返回写入的字符数。
     * 详见 emu_root_traps.h 的 TR_root_utf8_to_ucs2 注释与 zm_str.c 的实现
     * （旧实现按 memcpy 处理，是误判）。 */
    ret = zm_utf8_to_ucs2(uc, r0, r1, r2, r3);
    break;
  case TR_root_sprintf:
    /*
     * ROOT_TABLE_ADDR[0x6C] sprintf(dst=r0, fmt=r1, va_area=r2)
     *
     * zmaee 约定：r2 指向一个由 applet 自带"溢出变参"助手构造的参数区，
     * 布局为 [变参个数][第1个变参][第2个变参]...，每个变参占 **8 字节**
     * （值在槽首）。实测 00000506 sub_18EDC / 00001b62 同型助手：
     *   sub_98c90 扫描格式串，按 %d/%s/%x/%f... 逐个把 r2/r3/栈上的实参
     *   存到 slot = r6 + n*8 + 4，最后 strb 计数到 r6+0；随后调用本槽。
     * 因此这里必须用 U_VA_MEM8（addr = r2），而不是 4 字节连续布局——
     * 旧实现按 4 字节槽取参，第 2 个变参就会读到 0，sprintf 结果被截断
     * （实测 "%s\\%s" 只输出 "res\"，长度 4）。
     *
     * 换用 ulibc 后，格式串支持完整的 flags/width/precision/length 语法，
     * 且输出上限由 512 字节提高到 64KB。
     */
    {
      u_va va;
      u_va_start_mem8(&va, uc, r2, 0);
      ret = (uint32_t)u_sprintf(uc, r0, r1, &va);
      if (g_disasm) {
        char fmt[128], outp[256];
        uint32_t lr = 0;
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        read_cstr(uc, r1, fmt, sizeof(fmt));
        read_cstr(uc, r0, outp, sizeof(outp));
        log_debug("sprintf lr=0x%X [%u参数]: fmt=\"%s\" out=\"%s\" ret=%u", lr,
                  uc_read32(uc, r2), fmt, outp, ret);
      }
    }
    break;
  case TR_root_str_to_num:
    /* ROOT_TABLE_ADDR[0x74] = 数值字符串解析：strtol(str=r0, endptr=r1, base=r2)。
     * RE 详见 emu_root_traps.h 的 ZM_StrToNum 注释：帮助页的标记解析器用它把
     * "#FF0000" 拆成的三段两字符转成颜色分量，恒以 (buf, 0, 16) 调用；
     * 未接线时该槽返回 0，实测每轮报 3716 次"非法的外部调用"、颜色塌成黑。 */
    ret = (uint32_t)u_strtol(uc, r0, r1, (int)r2);
    break;
  case TR_root_str_ctor:
    /* ROOT_TABLE_ADDR[0x88] str_ctor(dst=r0, src=r1)：含 '\0' 一起拷，返回 dst */
    ret = u_strcpy(uc, r0, r1);
    break;
  case TR_root_strchr:
    /* ROOT_TABLE_ADDR[0x90] = zmaee_strlen(s=r0)，返回字符串长度（**不是** strchr）。
     *
     * 逆向证据（两个 applet 的实际用法一致，全是"长度"语义）：
     *   00000506 sub_313C/88AB8：sprintf("%s\\%s",...) 后取长度，作为
     *       IImage::SetData(0, name, len) 的 len → 再据此读文件；
     *   00001b62 0xB7DAC：长度 +1 后与缓冲上限比较，再调 strcpy 家族；
     *   00001b62 0xB91C0：长度 & 0xFF 当字节长度用。
     * 旧实现按 strchr 处理，遇到 r1 为残留脏值时返回 0，导致 applet 拿到
     * len=0 → malloc(0)/Read(0 字节) → 后续解引用野指针崩溃。 */
    ret = zm_strlen(uc, r0);
    break;
  case TR_root_memcmp:
    /* ROOT_TABLE_ADDR[0x50] memcmp(a=r0, b=r1, n=r2)
     * RE zmaee_memcmp @0x363E8（tramp 桩 0xb9b50）；00001b62 调用后
     * cmp r0,#0 判断，返回值当布尔用。 */
    ret = (uint32_t)u_memcmp(uc, r0, r1, r2);
    break;
  case TR_root_memcpy:
    /* ROOT_TABLE_ADDR[0x5C] memcpy(dst=r0, src=r1, n=r2)
     * RE zmaee_memcpy @0x36470（tramp 桩 0xb9b40）；00001b62 定长 4
     * 字节拷贝，返回值当 dst 用。 */
    ret = u_memcpy(uc, r0, r1, r2);
    break;
  case TR_root_memset:
    /* ROOT_TABLE_ADDR[0x60] memset(dst=r0, val=r1, len=r2) */
    ret = u_memset(uc, r0, r1, r2);
    break;
  case TR_root_str_assign:
    /* ROOT_TABLE_ADDR[0x78] str_assign(str_obj=r0, cstr=r1)：把 C 串赋给 zmaee
     * 字符串对象（写 data_ptr/len/内联缓冲三元组）。实现久备，
     * 此前因伪索引槽位冲突未接；00001b62 高频调用此槽。 */
    ret = zm_root_str_assign(uc, r0, r1);
    break;
  case TR_root_strstr:
    /* ROOT_TABLE_ADDR[0xB0] = zmaee_strstr(haystack=r0, needle=r1)。
     * 命中返回子串地址，未命中返回 0。 */
    ret = zm_strstr(uc, r0, r1);
    break;
  case TR_root_x68C:
    ret = zm_root_x68C(uc, r0, r1, r2, r3);
    break;
  case TR_root_spec_lookup:
    /* ROOT_TABLE_ADDR[0xA4] = zmaee_strpbrk(str=r0, charset=r1)。
     * applet 的 sprintf 包装（00000506 sub_98C90）用它统计格式串里的转换符
     * 个数，返回值必须是**原串内地址**（旧实现返回宿主缓冲，导致扫描指针
     * 跳飞、变参个数少算）。 */
    ret = zm_spec_lookup(uc, r0, r1);
    break;
  case TR_root_str_find:
    /*
     * ROOT_TABLE_ADDR[0xA8] str_find(str_obj_or_cstr=r0, ch=r1)
     * 仍是 zm_strchr：它带 zmaee 特有的"字符串对象 vs 裸 C 串"启发式
     * （判断 data_ptr 是否等于 ptr+12 的内联布局），属 zmaee 领域知识，
     * 不该塞进 ulibc。见 zm_str.c。
     */
    ret = zm_strchr(uc, r0, r1);
    break;
  /* ---- shell（g_aee_shell_vtbl @ .data:0x64440，34 槽）---- */
  case TR_shell_AddRef:
    ret = zm_shell_AddRef(uc, r0);
    break;
  case TR_shell_Release:
    ret = zm_shell_Release(uc, r0);
    break;
  case TR_shell_CreateInstance: /* 旧称 queryInterface：按服务号返回对象 */
    ret = zm_shell_CreateInstance(uc, r1, r2);
    break;
  case TR_shell_x0C: /* RE sub_34DE4，未知 */
    ret = zm_shell_stub(uc, 0x0C, r0, r1, r2, r3);
    break;
  case TR_shell_GetDeviceInfo: /* 旧称 getSystemInfo：写设备信息 */
    ret = zm_shell_GetDeviceInfo(uc, r1);
    break;
  case TR_shell_GetRootDir:
    ret = zm_shell_stub(uc, 0x14, r0, r1, r2, r3);
    break;
  case TR_shell_SetWorkDir:
    ret = zm_shell_stub(uc, 0x18, r0, r1, r2, r3);
    break;
  case TR_shell_GetWorkDir:
    ret = zm_shell_stub(uc, 0x1C, r0, r1, r2, r3);
    break;
  case TR_shell_StartApplet:
    ret = zm_shell_stub(uc, 0x20, r0, r1, r2, r3);
    break;
  case TR_shell_CloseApplet: /* +0x24 = CloseApplet(bRetToIdle)：applet 请求关闭 */
    ret = zm_shell_CloseApplet(uc, r1);
    break;
  case TR_shell_CanStartApplet:
    ret = zm_shell_stub(uc, 0x28, r0, r1, r2, r3);
    break;
  case TR_shell_ActiveApplet:
    ret = zm_shell_stub(uc, 0x2C, r0, r1, r2, r3);
    break;
  case TR_shell_GetApplet:
    /* RE（nativeAEERepaint）：GetApplet(shell, 0) 返回当前 applet 对象，
     * 随后调 (*applet_vt+8)(applet, 4, 0, 0) 做重绘。
     * g_instance 由 create_cbk 时记录（trap.c:238）。 */
    ret = zm_shell_GetApplet(uc, r1);
    break;
  case TR_shell_x34: /* RE sub_34764，未知 */
    ret = zm_shell_stub(uc, 0x34, r0, r1, r2, r3);
    break;
  case TR_shell_x38: /* RE sub_34C1C，未知 */
    ret = zm_shell_stub(uc, 0x38, r0, r1, r2, r3);
    break;
  case TR_shell_SetTimer: /* RE：a2=r1=时长ms cb=r2 owner=r3 a5=sp[0] */
    ret = zm_timer_SetTimer(uc, r1, r2, r3, uc_read32(uc, sp));
    break;
  case TR_shell_CancelTimer: /* RE：按 timer ID 取消 */
    ret = zm_timer_CancelTimer(uc, r1);
    break;
  case TR_shell_CancelOwnerTimer: /* RE：删全部 entry[1]==owner（r1）的定时器 */
    ret = zm_timer_CancelOwnerTimer(uc, r1);
    break;
  case TR_shell_GetTickCount: /* 单调毫秒时间戳 */
    ret = zm_shell_GetTickCount(uc);
    break;
  case TR_shell_OpenWapBrowser:
    ret = zm_shell_stub(uc, 0x4C, r0, r1, r2, r3);
    break;
  case TR_shell_x50: /* RE sub_3474C，未知 */
    ret = zm_shell_stub(uc, 0x50, r0, r1, r2, r3);
    break;
  case TR_shell_SetEndKeyMask:
    ret = zm_shell_stub(uc, 0x54, r0, r1, r2, r3);
    break;
  case TR_shell_LoadDLL: /* RE sub_35230：applet 实测传 dll 名 */
    ret = zm_shell_LoadDLL(uc, r1, r2, r3);
    break;
  case TR_shell_UnloadDLL: /* RE sub_346D8 */
    ret = zm_shell_UnloadDLL(uc, r1);
    break;
  case TR_shell_GetAppletMask:
    ret = zm_shell_stub(uc, 0x60, r0, r1, r2, r3);
    break;
  case TR_shell_SetAppletMask:
    ret = zm_shell_stub(uc, 0x64, r0, r1, r2, r3);
    break;
  case TR_shell_IsLoadGlobalLibrary:
    ret = zm_shell_stub(uc, 0x68, r0, r1, r2, r3);
    break;
  case TR_shell_LoadGlobalLibrary:
    ret = zm_shell_stub(uc, 0x6C, r0, r1, r2, r3);
    break;
  case TR_shell_FreeGlobalLibrary:
    ret = zm_shell_stub(uc, 0x70, r0, r1, r2, r3);
    break;
  case TR_shell_IsGlobalLibraryUseStaticMem:
    ret = zm_shell_stub(uc, 0x74, r0, r1, r2, r3);
    break;
  case TR_shell_LoadLibraryExt: /* 旧称 loadDLL2：载 zmsys006.dll */
    ret = zm_shell_LoadLibraryExt(uc, r0, r1, r2, r3);
    break;
  case TR_shell_EntryApplet:
    ret = zm_shell_stub(uc, 0x7C, r0, r1, r2, r3);
    break;
  case TR_shell_GetAppDir:
    ret = zm_shell_stub(uc, 0x80, r0, r1, r2, r3);
    break;
  case TR_shell_GetSupportHall:
    ret = zm_shell_stub(uc, 0x84, r0, r1, r2, r3);
    break;
  /* ---- fs / file（IFileMgr g_filemgr_vtbl @.data:0x64038，16 槽）---- */
  case TR_fileMgr_AddRef:
    ret = zm_fileMgr_stub(uc, 0x00, r0, r1, r2, r3);
    break;
  case TR_fileMgr_Release:
    ret = zm_fileMgr_stub(uc, 0x04, r0, r1, r2, r3);
    break;
  case TR_fileMgr_open_file:
    log_debug("fs_open(r0=0x%X)", r0); // 此处的 r0 是 FileMgr的地址
    log_debug("G_FileMgr_ADDR=0x%X", G_FileMgr_ADDR);

    if (r0 == 0) {
      ret = 0;
    } else {
      ret = zm_fileMgr_open_file(uc, r1); // 这地方为什么是r1呀
    }
    break;
  case TR_fileMgr_x0C: /* RE sub_2A550 */
    ret = zm_fileMgr_stub(uc, 0x0C, r0, r1, r2, r3);
    break;
  case TR_fileMgr_x10: /* RE sub_2A4E0 */
    ret = zm_fileMgr_stub(uc, 0x10, r0, r1, r2, r3);
    break;
  case TR_fileMgr_x14: /* RE sub_2A45C */
    ret = zm_fileMgr_stub(uc, 0x14, r0, r1, r2, r3);
    break;
  case TR_fileMgr_x18: /* RE sub_2A3F4 */
    ret = zm_fileMgr_stub(uc, 0x18, r0, r1, r2, r3);
    break;
  case TR_fileMgr_x1C: /* RE sub_2A344 */
    ret = zm_fileMgr_stub(uc, 0x1C, r0, r1, r2, r3);
    break;
  case TR_fileMgr_x20: /* RE sub_2A7BC：TestFile，存在 0/不存在 -1/参数 -4 */
    ret = zm_fileMgr_TestFile(uc, r0, r1);
    break;
  case TR_fileMgr_x24: /* RE sub_2A2D0 */
    ret = zm_fileMgr_stub(uc, 0x24, r0, r1, r2, r3);
    break;
  case TR_fileMgr_x28: /* RE sub_29EA0 */
    ret = zm_fileMgr_stub(uc, 0x28, r0, r1, r2, r3);
    break;
  case TR_fileMgr_x2C: /* RE sub_29E7C */
    ret = zm_fileMgr_stub(uc, 0x2C, r0, r1, r2, r3);
    break;
  case TR_fileMgr_x30: /* RE sub_29E40：存储区支持查询（'C'/'E'/'T'） */
    ret = zm_fileMgr_StorageSupport(uc, r0, r1);
    break;
  case TR_fileMgr_x34: /* RE sub_29E08 */
    ret = zm_fileMgr_stub(uc, 0x34, r0, r1, r2, r3);
    break;
  case TR_fileMgr_x38: /* RE sub_29E00 */
    ret = zm_fileMgr_stub(uc, 0x38, r0, r1, r2, r3);
    break;
  case TR_file_close:
    ret = zm_file_close(uc, r0);
    break; /* file.close(r0=this/file_id) */
  case TR_file_read:
    ret = zm_file_read(uc, r0, r1, r2);
    break;
  case TR_file_write: /* +0x0C = ZMAEE_IFile_Write(ifile, buf, len)：applet 存档 */
    ret = zm_file_write(uc, r0, r1, r2);
    break;
  case TR_file_seek:
    ret = zm_file_seek(uc, r0, r1, r2);
    break;
  case TR_file_tell:
    /*
     * FILE_VT_ADDR[0x24] = ZMAEE_IFile_Tell（逆向实测），返回当前读写位置。
     * 取文件总大小的惯用法是 Seek(0,SEEK_END) 后调用本槽位。
     */
    ret = zm_file_tell(uc, r0);
    break;
  /* ---- ZMAEE IDisplay 原生虚表（g_aee_display_vtbl）---- */
  case TR_util_x00: ret = zm_util_stub(uc, 0x00, r0, r1, r2, r3); break;
  case TR_util_x04: ret = zm_util_stub(uc, 0x04, r0, r1, r2, r3); break;
  case TR_util_x08: ret = zm_util_stub(uc, 0x08, r0, r1, r2, r3); break;
  case TR_util_x0C: ret = zm_util_stub(uc, 0x0C, r0, r1, r2, r3); break;
  case TR_util_x10: ret = zm_util_stub(uc, 0x10, r0, r1, r2, r3); break;
  case TR_util_x14: ret = zm_util_stub(uc, 0x14, r0, r1, r2, r3); break;
  case TR_util_x18: ret = zm_util_stub(uc, 0x18, r0, r1, r2, r3); break;
  case TR_display_AddRef:
    ret = zm_display_AddRef(uc, r0);
    break;
  case TR_display_Release:
    ret = zm_display_Release(uc, r0);
    break;
  case TR_display_GetMaxLayerCount:
    ret = zm_display_GetMaxLayerCount(uc, r0);
    break;
  case TR_display_CreateLayer:
    ret = zm_display_CreateLayer(uc, 0x0CU, r0, r1, r2, r3);
    break;
  case TR_display_CreateLayerExt:
    ret = zm_display_CreateLayerExt(uc, 0x10U, r0, r1, r2, r3);
    break;
  case TR_display_FreeLayer:
    ret = zm_display_FreeLayer(uc, r0, r1);
    break;
  case TR_display_FreeAllLayer: /* +0x18：按真机反编译实现（清活动层 + 释放层 1..15） */
    ret = zm_display_FreeAllLayer(uc, r0);
    break;
  case TR_display_GetLayerInfo:
    ret = zm_display_GetLayerInfo(uc, 0x1CU, r0, r1, r2, r3);
    break;
  case TR_display_SetActiveLayer: /* 真机虚表 +0x20 = SetActiveLayer(display, idx) */
    ret = zm_display_SetActiveLayer(uc, r0, r1);
    break;
  case TR_display_SetLayerPosition:
    ret = zm_display_SetLayerPosition(uc, 0x24U, r0, r1, r2, r3);
    break;
  case TR_display_Update:
    /* RE：Update(display, x, y, w, h) —— x/y/w 在 r1/r2/r3，h 在第 5 个参数。 */
    ret = zm_display_Update(uc, r0, r1, r2, r3, getArg(uc, 4));
    break;
  case TR_display_UpdateEx: /* 真机虚表 +0x2C = UpdateEx(display, rect, count, layerList)
                             * 四个参数正好在 r0~r3，无需取栈参数。 */
    ret = zm_display_UpdateEx(uc, r0, r1, r2, r3);
    break;
  case TR_display_GetActiveLayer:
    ret = zm_display_GetActiveLayer(uc, r0);
    break;
  case TR_display_LockScreen: /* 真机虚表 +0x34 = LockScreen（实测被调，功能未知） */
    ret = zm_display_stub(uc, 0x34, r0, r1, r2, r3);
    break;
  case TR_display_UnlockScreen:
    ret = zm_display_UnlockScreen(uc, r0);
    break;
  case TR_display_RegisterCustomFont:
    ret = zm_display_RegisterCustomFont(uc, 0x3CU, r0, r1, r2, r3);
    break;
  case TR_display_SelectFont: /* 真机虚表 +0x40 = SelectFont(this, idx) */
    ret = zm_display_SelectFont(uc, r0, r1);
    break;
  case TR_display_GetFontWidth:
    ret = zm_display_GetFontWidth(uc, r0, r1);
    break;
  case TR_display_GetFontHeight: /* 真机虚表 +0x48 = GetFontHeight（实现待 RE 校准） */
    ret = zm_display_GetFontHeight(uc);
    break;
  case TR_display_MeasureString: /* 真机虚表 +0x4C = MeasureString(disp, str_ptr, len, width_out, sp[metrics]) */
    ret = zm_display_MeasureString(uc, r0, r1, r2, r3, sp);
    break;
  case TR_display_DrawText:
    ret = zm_display_DrawText(uc, r1, r2, r3, sp);
    break;
  case TR_display_SetTransColor:
    ret = zm_display_SetTransColor(uc, r0, r1, r2);
    break;
  case TR_display_SetOpacity:
    ret = zm_display_SetOpacity(uc, 0x58U, r0, r1, r2, r3);
    break;
  case TR_display_SetClipRect:
    ret = zm_display_SetClipRect(uc, 0x5CU, r0, r1, r2, r3);
    break;
  case TR_display_GetClipRect:
    ret = zm_display_GetClipRect(uc, 0x60U, r0, r1, r2, r3);
    break;
  case TR_display_SetPixel:
    ret = zm_display_SetPixel(uc, 0x64U, r0, r1, r2, r3);
    break;
  case TR_display_DrawLine:
    ret = zm_display_DrawLine(uc, 0x68U, r0, r1, r2, r3);
    break;
  case TR_display_DrawRect:
    ret = zm_display_DrawRect(uc, r1, r2, r3, sp);
    break;
  case TR_display_FillRect:
    ret = zm_display_FillRect(uc, r1, r2, r3, sp);
    break;
  case TR_display_DrawRoundRect:
    ret = zm_display_DrawRoundRect(uc, 0x74U, r0, r1, r2, r3);
    break;
  case TR_display_DrawCircle:
    ret = zm_display_DrawCircle(uc, 0x78U, r0, r1, r2, r3);
    break;
  case TR_display_FillCircle:
    ret = zm_display_FillCircle(uc, 0x7CU, r0, r1, r2, r3);
    break;
  case TR_display_DrawArc:
    ret = zm_display_DrawArc(uc, 0x80U, r0, r1, r2, r3);
    break;
  case TR_display_FillArc:
    ret = zm_display_FillArc(uc, 0x84U, r0, r1, r2, r3);
    break;
  case TR_display_FillGradientRect:
    ret = zm_display_FillGradientRect(uc, 0x88U, r0, r1, r2, r3);
    break;
  case TR_display_AlphaBlendRect:
    ret = zm_display_AlphaBlendRect(uc, 0x8CU, r0, r1, r2, r3);
    break;
  case TR_display_DrawImage:
    ret = zm_display_DrawImage(uc, 0x90U, r0, r1, r2, r3);
    break;
  case TR_display_DrawBitmap:
    ret = zm_display_DrawBitmap(uc, 0x94U, r0, r1, r2, r3);
    break;
  case TR_display_DrawBitmapEx:
    if (g_disasm) {
      static int watch_done = 0;
      if (!watch_done && r3 >= 0x820000) {
        uint32_t b0 = uc_read32(uc, r3), b1 = uc_read32(uc, r3 + 4);
        uint32_t pc0 = 0;
        uc_reg_read(uc, UC_ARM_REG_R4, &pc0);
        log_debug("WATCH bmp=0x%X first=[%08X %08X] R4=0x%X", r3, b0, b1, pc0);
        watch_done = 1;
      }
      uint32_t b0 = uc_read32(uc, r3), b1 = uc_read32(uc, r3 + 4);
      uint32_t b2 = uc_read32(uc, r3 + 8);
      log_debug("DrawBitmapEx lr=0x%X x=%u y=%u bmp=0x%X(=[%08X %08X %08X]) "
                "rect=0x%X mode=%u",
                lr, r1, r2, r3, b0, b1, b2, getArg(uc, 4), getArg(uc, 5));
    }
    ret = zm_display_DrawBitmapEx(uc, 0x98U, r0, r1, r2, r3);
    break;
  case TR_display_DrawBitmapFrame:
    ret = zm_display_DrawBitmapFrame(uc, 0x9CU, r0, r1, r2, r3);
    break;
  case TR_display_CreateBitmap:
    ret = zm_display_CreateBitmap(uc, r0, r1, r2, r3);
    break;
  case TR_display_LoadBitmap:
    ret = zm_display_LoadBitmap(uc, r0, r1);
    break;
  case TR_display_CreateImage:
    ret = zm_display_CreateImage(uc, 0xA8U, r0, r1, r2, r3);
    break;
  case TR_display_BitBlt:
    if (g_disasm) {
      uint32_t s0 = uc_read32(uc, r3), s1 = uc_read32(uc, r3 + 4);
      uint32_t s2 = uc_read32(uc, r3 + 8), s3 = uc_read32(uc, r3 + 12);
      log_debug("BitBlt lr=0x%X dx=%u dy=%u surf=0x%X(=[%08X %08X %08X %08X]) "
                "rect=0x%X mode=%u flags=%u",
                lr, r1, r2, r3, s0, s1, s2, s3, getArg(uc, 4), getArg(uc, 5),
                getArg(uc, 6));
    }
    ret = zm_display_BitBlt(uc, 0xACU, r0, r1, r2, r3);
    break;
  case TR_display_Flatten:
    ret = zm_display_Flatten(uc, 0xB0U, r0, r1, r2, r3);
    break;
  case TR_display_StretchBlt: {
    static uint32_t sn = 0;
    if (sn++ < 6)
      log_info("[StretchBlt] lr=0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X", lr, r0, r1,
               r2, r3);
    ret = zm_display_StretchBlt(uc, 0xB4U, r0, r1, r2, r3);
    break;
  }
    break;
  case TR_display_DrawAntialiasingLine:
    ret = zm_display_DrawAntialiasingLine(uc, 0xB8U, r0, r1, r2, r3);
    break;
  case TR_display_DrawWLine:
    ret = zm_display_DrawWLine(uc, 0xBCU, r0, r1, r2, r3);
    break;
  case TR_display_GetDMLayerHdlr:
    ret = zm_display_GetDMLayerHdlr(uc, 0xC0U, r0, r1, r2, r3);
    break;
  case TR_display_RelevanceLayer:
    ret = zm_display_RelevanceLayer(uc, 0xC4U, r0, r1, r2, r3);
    break;
  case TR_display_Refresh:
    ret = zm_display_Refresh(uc);
    break;
  case TR_display_DrawImageExt:
    ret = zm_display_DrawImageExt(uc, 0xCCU, r0, r1, r2, r3);
    break;
  case TR_display_DrawSysWallPaper:
    ret = zm_display_DrawSysWallPaper(uc, 0xD0U, r0, r1, r2, r3);
    break;
  case TR_display_DrawBorderText:
    ret = zm_display_DrawBorderText(uc, 0xD4U, r0, r1, r2, r3);
    break;
  case TR_display_PushAndSetAlphaLayer:
    ret = zm_display_PushAndSetAlphaLayer(uc, 0xD8U, r0, r1, r2, r3);
    break;
  case TR_display_PopAndRestoreAlphaLayer:
    ret = zm_display_PopAndRestoreAlphaLayer(uc, 0xDCU, r0, r1, r2, r3);
    break;
  case TR_display_RotateScreen:
    ret = zm_display_RotateScreen(uc, 0xE0U, r0, r1, r2, r3);
    break;
  /* ---- ZMAEE IBitmap 原生虚表（g_aee_bitmap_vtbl）---- */
  /* ---- ZMAEE IImage（IDisplay::CreateImage 造出的解码器对象）---- */
  /* ---- ZMAEE surface 门面（IImage::Decode 的 out 对象）---- */
  case TR_surf_release:
    ret = zm_surf_release(uc, r0);
    break;
  case TR_surf_getrect:
    ret = zm_surf_getrect(uc, r0, r1);
    break;
  case TR_surf_x04:
  case TR_surf_x08:
  case TR_surf_x0C:
  case TR_surf_x14:
  case TR_surf_x18:
  case TR_surf_x1C:
  case TR_surf_x20:
  case TR_surf_x24:
  case TR_surf_x28:
  case TR_surf_x2C:
  case TR_surf_x30:
  case TR_surf_x34:
  case TR_surf_x38:
  case TR_surf_x3C:
  case TR_surf_x40:
  case TR_surf_x44:
  case TR_surf_x48:
  case TR_surf_x4C:
  case TR_surf_x50:
    ret = zm_surf_nop(uc, trap_address - TRAMP_BASE);
    break;
  case TR_image_AddRef:
    ret = zm_image_AddRef(uc, r0);
    break;
  case TR_image_Release:
    ret = zm_image_Release(uc, r0);
    break;
  case TR_image_SetData:
    ret = zm_image_SetData(uc, r0, r1, r2, r3);
    break;
  case TR_image_GetFrameCount:
    ret = zm_image_GetFrameCount(uc, r0);
    break;
  case TR_image_Width:
    ret = zm_image_Width(uc, r0);
    break;
  case TR_image_Height:
    ret = zm_image_Height(uc, r0);
    break;
  case TR_image_GetType:
    ret = zm_image_GetType(uc, r0);
    break;
  case TR_image_Decode:
    ret = zm_image_DecodeToBitmap(uc, r0, r1, r2, r3, sp);
    break;
  case TR_image_x20:
  case TR_image_x24:
  case TR_image_x28:
  case TR_image_x2C:
  case TR_image_x30:
  case TR_image_x34:
  case TR_image_x38:
  case TR_image_x3C:
    ret = zm_image_stub(uc, trap_address - TRAMP_BASE, r0, r1, r2, r3);
    break;
  case TR_bitmap_AddRef:
    ret = zm_bitmap_AddRef(uc, r0);
    break;
  case TR_bitmap_Release:
    ret = zm_bitmap_Release(uc, r0);
    break;
  case TR_bitmap_SetTransColor:
    ret = zm_bitmap_SetTransColor(uc, r0, r1);
    break;
  case TR_bitmap_sub_25F78:
    ret = zm_bitmap_sub_25F78(uc, 0x0CU, r0, r1, r2, r3);
    break;
  case TR_bitmap_GetInfo:
    ret = zm_bitmap_GetInfo(uc, r0, r1);
    break;
  case TR_bitmap_sub_25F84:
    ret = zm_bitmap_sub_25F84(uc, 0x14U, r0, r1, r2, r3);
    break;
  case TR_bitmap_sub_25FF8:
    ret = zm_bitmap_sub_25FF8(uc, 0x18U, r0, r1, r2, r3);
    break;
  /* ---- ZMAEE IMedia（音频，0x100000C，g_aee_media_vtbl，25 槽）---- */
  case TR_media_AddRef:
    log_info("IMedia.AddRef called");
    ret = 1; /* 单例 */
    break;
  case TR_media_Release:
    log_info("IMedia.Release called");
    ret = zm_svc_release(uc);
    break;
  case TR_media_x08:
    ret = zm_media_stub(uc, 0x08, r0, r1, r2, r3);
    break;
  case TR_media_x0C:
    ret = zm_media_stub(uc, 0x0C, r0, r1, r2, r3);
    break;
  case TR_media_play: /* +0x10：命令分发器 loc_32E80（cmd=r1 低16位，第5参数起在栈上） */
    ret = zm_media_command(uc, r0, r1, r2, r3, sp);
    break;
  case TR_media_stop: /* +0x14：停止播放 */
    ret = zm_media_stop(uc);
    break;
  case TR_media_x18: /* +0x18 = pauseMusic（JNI 同名），applet"声音关"靠它 */
    ret = zm_media_pause_music(uc);
    break;
  case TR_media_x1C: /* +0x1C = resumeMusic（JNI 同名），applet"声音开"靠它 */
    ret = zm_media_resume_music(uc);
    break;
  case TR_media_x20:
    ret = zm_media_stub(uc, 0x20, r0, r1, r2, r3);
    break;
  case TR_media_x24:
    ret = zm_media_stub(uc, 0x24, r0, r1, r2, r3);
    break;
  case TR_media_x28:
    ret = zm_media_stub(uc, 0x28, r0, r1, r2, r3);
    break;
  case TR_media_x2C:
    ret = zm_media_stub(uc, 0x2C, r0, r1, r2, r3);
    break;
  case TR_media_x30:
    ret = zm_media_stub(uc, 0x30, r0, r1, r2, r3);
    break;
  case TR_media_x34:
    ret = zm_media_stub(uc, 0x34, r0, r1, r2, r3);
    break;
  case TR_media_x38: /* RE sub_32BDC：固定返回 -1 */
    ret = (uint32_t)-1;
    break;
  case TR_media_x3C:
    ret = zm_media_stub(uc, 0x3C, r0, r1, r2, r3);
    break;
  case TR_media_x40: /* RE sub_32BE8：固定返回 -1 */
    ret = (uint32_t)-1;
    break;
  case TR_media_x44:
    ret = zm_media_stub(uc, 0x44, r0, r1, r2, r3);
    break;
  case TR_media_x48:
    ret = zm_media_stub(uc, 0x48, r0, r1, r2, r3);
    break;
  case TR_media_x4C:
    ret = zm_media_stub(uc, 0x4C, r0, r1, r2, r3);
    break;
  case TR_media_x50:
    ret = zm_media_stub(uc, 0x50, r0, r1, r2, r3);
    break;
  case TR_media_x54: /* +0x54：sub_33128 = 同一分发器的 thunk */
    ret = zm_media_command(uc, r0, r1, r2, r3, sp);
    break;
  case TR_media_x58:
    ret = zm_media_stub(uc, 0x58, r0, r1, r2, r3);
    break;

  /* ---- ZMAEE ISetting（0x100000B，g_aee_setting_vtbl，14 槽）---- */
  case TR_setting_AddRef:
    ret = 1; /* 单例 */
    break;
  case TR_setting_Release:
    ret = zm_svc_release(uc);
    break;
  case TR_setting_x08:
    ret = zm_setting_stub(uc, 0x08, r0, r1, r2, r3);
    break;
  case TR_setting_x0C:
    ret = zm_setting_stub(uc, 0x0C, r0, r1, r2, r3);
    break;
  case TR_setting_x10:
    ret = zm_setting_stub(uc, 0x10, r0, r1, r2, r3);
    break;
  case TR_setting_x14:
    ret = zm_setting_stub(uc, 0x14, r0, r1, r2, r3);
    break;
  case TR_setting_x18: /* +0x18 = 声音开关（键 "on"），RE 见 zm_shell.c */
    /* 调用点一定要记：applet 里"开始"用 on=1、"停止"用 on=0（+pauseMusic），
     * 排查"开关方向不对"时全靠这个 lr 定位是哪支函数在说话。 */
    log_info("ISetting[0x18] lr=0x%X on=%u", lr, r1);
    ret = zm_setting_set_sound(uc, r1);
    break;
  case TR_setting_x1C:
    if (g_disasm)
      log_debug("ISetting[0x1C] 调用点 lr=0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X",
                lr, r0, r1, r2, r3);
    ret = zm_setting_stub(uc, 0x1C, r0, r1, r2, r3);
    break;
  case TR_setting_x20:
    if (g_disasm)
      log_debug("ISetting[0x20] 调用点 lr=0x%X r0=0x%X r1=0x%X r2=0x%X r3=0x%X",
                lr, r0, r1, r2, r3);
    ret = zm_setting_stub(uc, 0x20, r0, r1, r2, r3);
    break;
  case TR_setting_x24: /* 保留既有"写 0"行为，applet 依赖其分支判断 */
    ret = zm_setting_x24(uc, r1, r2);
    break;
  case TR_setting_x28:
    ret = zm_setting_stub(uc, 0x28, r0, r1, r2, r3);
    break;
  case TR_setting_x2C:
    ret = zm_setting_stub(uc, 0x2C, r0, r1, r2, r3);
    break;
  case TR_setting_x30:
    ret = zm_setting_stub(uc, 0x30, r0, r1, r2, r3);
    break;
  case TR_setting_x34:
    ret = zm_setting_stub(uc, 0x34, r0, r1, r2, r3);
    break;

  /* ---- 服务对象（IShell.CreateInstance 返回）/ FS / DLL / CBK ---- */
  case TR_netmgr_release:
    ret = zm_svc_release(uc);
    break;
  case TR_netmgr_x1C:
    ret = zm_netmgr_x1C(uc, r0, r1, r2, r3);
    break;
  case TR_tapi_x00:
  case TR_tapi_x08:
  case TR_tapi_x0C:
  case TR_tapi_x10:
  case TR_tapi_x14:
  case TR_tapi_x18:
  case TR_tapi_x1C:
  case TR_tapi_x20:
  case TR_tapi_x24:
  case TR_tapi_x28:
  case TR_tapi_x30:
  case TR_tapi_x34:
  case TR_tapi_x38:
  case TR_tapi_x3C:
  case TR_tapi_x44:
  case TR_tapi_x48:
  case TR_tapi_x4C:
  case TR_tapi_x50:
    ret = zm_tapi_stub(uc, trap_address - TAPI_VT_ADDR, r0, r1, r2, r3);
    break;
  case TR_tapi_release:
    ret = zm_svc_release(uc);
    break;
  case TR_tapi_x2C:
    ret = zm_tapi_x2C(uc, r0, r1, r2, r3);
    break;
  case TR_tapi_x40:
    ret = zm_tapi_x40(uc, r0, r1, r2, r3);
    break;

  /* ZMAEE IZip 虚表（ZIP_VT_ADDR，5 槽） */
  case TR_zip_x00:
  case TR_zip_x04:
  case TR_zip_x08:
  case TR_zip_x0C:
  case TR_zip_x10:
    ret = zm_zip_stub(uc, trap_address - ZIP_VT_ADDR, r0, r1, r2, r3);
    break;
  case TR_dll_release: /* +0x04 = Release（00000506 付费流程实测）→ 与 NetMgr/Tapi 同款 */
    ret = zm_svc_release(uc);
    break;
  case TR_dll_init:
    ret = zm_dll_init(uc);
    break;
  case TR_dll_config:
    ret = zm_dll_config(uc, r1, r2, r3);
    break;
  case TR_dll_entry:
    ret = zm_dll_entry(uc, r1, r2, r3);
    break;
  case TR_cbk_default:
    ret = zm_root_cbk_default(uc, r0, r1, r2, r3);
    break;
  case TR_root_wcslen:
    /* ROOT_TABLE_ADDR[0xD8] = zmaee_wcslen(ptr=r0)：宽字符串长度（字符数）。
     * 旧实现返回 SDL_GetTicks（猜测），实测调用点全是"取长度"，详见
     * emu_root_traps.h 与 zm_str.c 的 zm_wcslen 注释。 */
    ret = zm_wcslen(uc, r0);
    break;
  case TR_root_create_cbk:
    /*
     * ROOT_TABLE_ADDR[0x154]：返回回调对象 CBK_OBJ（其 vt[+8] 随后会被 applet 覆写）。
     * 这是 zmaee 领域语义而非 libc，故走 zm_root；此前实现被注释掉，
     * 导致 applet 00000440 在真实堆下走到此处时报"非法的外部调用"。
     */
    ret = zm_root_create_cbk(uc);
    break;
  case TR_root_srand:
    zm_root_srand(r0);
    ret = 0;
    break;
  case TR_root_rand:
    ret = zm_root_rand();
    break;
  case TR_root_sqrt:
    ret = zm_root_math(uc, ZM_MATH_SQRT, r0, r1);
    break;
  case TR_root_cos:
    ret = zm_root_math(uc, ZM_MATH_COS, r0, r1);
    break;
  case TR_root_sin:
    ret = zm_root_math(uc, ZM_MATH_SIN, r0, r1);
    break;
  case TR_root_atan:
    ret = zm_root_math(uc, ZM_MATH_ATAN, r0, r1);
    break;
  case TR_root_tan:
    ret = zm_root_math(uc, ZM_MATH_TAN, r0, r1);
    break;
  default:
    /* ROOT_TABLE_ADDR 槽位尚未接线。打印调用现场寄存器，便于按参数签名反推该槽
     * 对应的 libc 函数（ROOT_TABLE_ADDR 在安卓变体里是普通全局函数指针表，
     * 无 g_aee_root_vtbl 符号可查）。 */
    if (trap_address >= TRAMP_BASE &&
        trap_address < TRAMP_BASE + TRAMP_SIZE) {
      uint32_t slot = trap_address - TRAMP_BASE;
      log_error("非法的外部调用: 0x%08X (SHIM槽+0x%X) r0=0x%X r1=0x%X "
                "r2=0x%X r3=0x%X sp[0]=0x%X sp[4]=0x%X lr=0x%X",
                trap_address, slot, r0, r1, r2, r3, uc_read32(uc, sp),
                uc_read32(uc, sp + 4), lr);
    } else {
      log_error("非法的外部调用: 0x%08" PRIx32, trap_address);
    }
    // pause_console(); // 仅注释掉阻塞，让模拟器继续往下跑（暴露更深的下一层）
    break;
  }

  log_debug("applet 调用外部的返回值是 r0等于0x%X", ret);

  uc_reg_write(uc, UC_ARM_REG_R0, &ret);

  uc_reg_write(uc, UC_ARM_REG_PC, &lr);
}

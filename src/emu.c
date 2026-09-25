#include "./emu.h"
#include "./hook.h"
#include "./trap.h" /* trap_dump_recent：崩溃时打"最近的外部调用" */
#include "./log/log.h"
#include "./tool/odds.h" /* get_filename_from_fullpath */
#include "./tool/uc_helper.h"
#include "./ulibc/ulibc.h"
#include "./zmaee/fs/zm_file_mgr.h"
#include "./zmaee/fs/zm_file.h"
#include "./zmaee/gfx/zm_display.h" /* zm_display_size（初始化 CBK_CTX 用） */
#include "./zmaee/gfx/zm_layer.h"   /* zm_layer_init_base（建立层 0） */
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

/* 运行期层/窗口尺寸：真源是 .app 头部的主屏尺寸，main.c 解析头部后写入。
 * 默认值仅作兜底；容量上限见 LAYER_MAX_W/H。 */
int g_layer_w = 240;
int g_layer_h = 320;
uint32_t g_registered_loop = 0;
int g_trap_pause = 0;
int g_disasm = 0; /* 默认关闭；ZM_DISASM=1 打开（会刷大量反汇编日志）*/

csh g_cs_handle;
cs_insn *g_sc_insn;
size_t g_sc_count;
uint8_t g_cscode[16];

/* -------------------- 实现 -------------------- */

/**
 * @brief 把 ZM_CRASH_MEM 指定的客户机内存区间转储到 /tmp/zm_crash_mem<N>_<地址>.bin
 *
 * 【为什么必须有】有些 applet（实测 000004dc《仙剑奇侠传》）的 .app 在磁盘上是
 * **加密/压缩**的：运行时内存里的指令与文件字节**完全不同** —— 同一地址，文件里
 * 解出来是 `pop {…,pc}`，内存里其实是 `blx r2` ✗。没有这个转储就只能对着错的
 * 指令流推理（为此白绕过很久）。
 *
 * 两个触发点：① 崩溃时；② **正常停止时**（ZM_DUMP_ON_EXIT=1）—— 后者专门对付
 * "不崩但也不干活"的 applet：实测 000004dc 就是无限跑 1ms 定时器、从不 Update，
 * 要看清那个回调在等什么，必须把运行时的代码/数据 dump 出来离线读。
 *
 * 格式：ZM_CRASH_MEM="0x起址:0x长度[;0x起址:0x长度…]"，长度省略默认 0x100。
 */
static void emu_dump_mem_ranges(const char *why) {
  const char *cm = getenv("ZM_CRASH_MEM");
  if (!cm || !cm[0])
    return;
  const char *p = cm;
  for (int idx = 0; idx < 8 && *p;) {
    unsigned long a = strtoul(p, (char **)&p, 0);
    unsigned long len = 0x100;
    if (*p == ':')
      len = strtoul(p + 1, (char **)&p, 0);
    while (*p == ';' || *p == ',')
      p++;
    if (len == 0 || len > 0x400000)
      len = 0x100;
    char fn[64];
    snprintf(fn, sizeof(fn), "/tmp/zm_crash_mem%d_%08lX.bin", idx, a);
    FILE *fp = fopen(fn, "wb");
    if (fp) {
      uint8_t *tmp = malloc(len);
      if (tmp) {
        if (uc_mem_read(g_uc, a, tmp, len) == UC_ERR_OK)
          fwrite(tmp, 1, len, fp);
        else
          log_error("内存转储: 0x%lX 读不出来（未映射？）", a);
        free(tmp);
      }
      fclose(fp);
      log_error("内存转储（%s）: %s（0x%lX 起，%lu 字节）", why, fn, a, len);
    }
    idx++;
  }
}

int zm_emu_map_memory() {
  uc_err err;
  /* 逐个检查：原实现把 6 次映射的返回值连续赋给同一个 err，只验最后一次，
   * 导致前面任何一次映射失败都被后面的成功覆盖、被静默吞掉。这里每次映射后
   * 立即判错并返回，避免“映射残缺却继续跑”的难查崩溃。 */
#define ZM_MAP(base, size)                                                    \
  do {                                                                        \
    err = uc_mem_map(g_uc, (base), (size), UC_PROT_ALL);                       \
    if (err != UC_ERR_OK) {                                                   \
      log_error("uc_mem_map(" #base ") failed, err: %d\n", err);              \
      return -1;                                                              \
    }                                                                         \
  } while (0)

  ZM_MAP(BLOB_BASE, BLOB_SIZE);
  ZM_MAP(STACK_BASE, STACK_SIZE);
  /* CBK 自管堆专用区（见 emu_mem_regions.h：不能用 blob 区，会撞 applet 的 BSS） */
  ZM_MAP(CBKHEAP_BASE, CBKHEAP_SIZE);
  /* CBK 堆之上的跳板页：页首放"返回 0"的桩（见 emu_mem_regions.h 的说明），
   * 它同时充当分配器的"区末上界"与 applet 会 blx 过去的函数指针。 */
  ZM_MAP(CBK_STUB_BASE, CBK_STUB_SIZE);
  ZM_MAP(CBK_TAIL_BASE, CBK_TAIL_SIZE); /* 跳板页之后的暂存垫子（见头注释） */
  ZM_MAP(HEAP_BASE, HEAP_SIZE);
  ZM_MAP(SHIM_BASE, SHIM_SIZE);
  ZM_MAP(TRAMP_BASE, TRAMP_SIZE);
#undef ZM_MAP

  /* 跳板页页首的桩：**返回一个"中性假对象"**。
   *
   * 为什么不是"返回 0/1"：调用方是两条不同的路 ——
   *   00000710 家族：`r2 = [[CBK+0x4C]]+0x30`，结果**按字节当 bool** 用 → 非 0 即真 ✓
   *   0000050b 家族：`r4 = f(this=分配器, buf, len=12)`，然后
   *       `movs r4,r0; beq <失败>` → 非 0 继续 ✓
   *       `ldr r0,[r4]; ldr r3,[r0,#0xc]; blx r3`  ← **把返回值当对象用** ✗
   *     实测：返 0 走失败分支 → 后面跳野地址崩；返 1（非 0 但不可解引用）
   *       → 它去读 [1+0xc] 又崩。所以必须给一个**能当对象解引用**的指针。
   *
   * 做法（页内布局）：
   *   +0x000  本桩：ldr r0,[pc,#0]（取 +8 的字）; bx lr; .word 假对象地址
   *   +0x100  堆管理器（[管理器+8] 也指着页首，兼作"区末上界"）
   *   +0x200  假虚表：16 个槽全指向本桩（每个"方法"调用后返回同一个假对象 ✓）
   *   +0x300  假对象：[0] = 假虚表
   * 指令用 `bh`/`bx` 兼容写法（纯 ARM、`bx lr` 按 lr 最低位切回原模式 ✓）。 */
  {
    uint32_t vtab = CBK_STUB_BASE + 0x200u;
    uint32_t obj = CBK_STUB_BASE + 0x300u;
    /* 桩：ldr r0,[pc,#0]; bx lr; .word obj */
    uint32_t code[3] = {0xE59F0000u, 0xE12FFF1Eu, obj};
    uc_mem_write(g_uc, CBK_STUB_BASE, code, sizeof(code));
    /* 假虚表：给足 0x100 字节（64 槽）—— 这类 applet 调到的槽偏移不固定
     * （0000050b 用 +0xc、00000710 用 +0x30、还有别处用 +4），表太小会读到表外。
     * ★ 但**不能每一槽都返回到"假对象"**：实测 0000050b 会拿某个槽的返回值当
     *   **尺寸**用（`vt[0x24]` → `malloc(返回値)`），而"假对象地址"是个指针 ✗
     *   —— 它于是 malloc(0xFD0300 = 16MB) → 失败 → NULL → 后面把尺寸当指针 → 崩。
     *   所以：+0x24 这类"取尺寸/计数"槽 → 填"返回 0"的桩 ✓（0 尺寸 = 没有内容，
     *   applet 走"空"分支，不会去访问不存在的缓冲）；其余槽仍返回假对象 ✓
     *   （有些调用点要的就是一个能继续解引用的对象）。 */
    uint32_t stub_obj = CBK_STUB_BASE;      /* 返回假对象 */
    uint32_t stub_zero = CBK_STUB_BASE + 0x10u; /* 返回 0 */
    static const uint8_t zero8[8] = {0x00, 0x00, 0xA0, 0xE3, 0x1E, 0xFF, 0x2F, 0xE1};
    uc_mem_write(g_uc, stub_zero, zero8, sizeof(zero8));
    for (uint32_t o = 0; o < 0x100u; o += 4) {
      uint32_t fn = (o == 0x24u) ? stub_zero : stub_obj;
      uc_mem_write(g_uc, vtab + o, &fn, 4);
    }
    /* 假对象：[0] = 假虚表 */
    uc_mem_write(g_uc, obj, &vtab, 4);

    /* ★ 文件对象虚表：给 [CBK+0x4C]→[+0] 的 +8 那条"打开资源文件"路用
     * （见 emu_mem_regions.h 的 CBK_TRAP_BASE 与 zm_cbk_file.h 的长注释）。
     *   +0x04 release / +0x08 read / +0x0C write / +0x20 seek / +0x24 **文件大小**
     * 其余槽一律"返回 0" —— 绝不再让"指针当尺寸"重演 ✗。 */
    /* ★★ 陷阱窗口要**同时当头"相对偏移表"**（实测 000004dc《仙剑》才看清）：
     * 这族 applet 的虚调用是**相对**的 ——
     *     ldr r1,[obj,#8]      ; r1 = 表首
     *     ldr r2,[r1,#0x1c]    ; r2 = 表首 +0x1C 处的**偏移**（可正可负）
     *     add ip,r2,r1         ; 目标 = 表首 + 偏移
     *     blx ip
     * 它们对我们给出的 [管理器+8] 也这么算：目标 = 0xFD0040 + *(0xFD005C) ✗
     * —— 那里原来是**没初始化**的字节 → 跳到 0x128334DC 之类野地址 ✓。
     * 而陷阱是**按 PC 拦截**的，窗口里的字节内容对"直接调用"毫无影响 ✓，所以这里
     * 把窗口清成 0：任何槽读出的偏移都是 0 ⇒ 目标 = CBK_TRAP_BASE + 0 = 我们的
     * "打开文件"陷阱 ✓（它拿到的 r1 不是路径时会安静地返回 0，不会造成伤害）。
     * 备注：文件对象虚表（CBK_FILE_VT）里存的是**绝对**陷阱地址 ✗，只适用于
     * 绝对约定的那几族（0000050b 实测可用 ✓）；相对约定的 applet 走到那里会跳飞，
     * 但实测它们还没走到那一步。 */
    {
      uint32_t zeros[0x40 / 4] = {0};
      uc_mem_write(g_uc, CBK_TRAP_BASE, zeros, sizeof(zeros));
    }

    for (uint32_t o = 0; o < 0x40u; o += 4)
      uc_mem_write(g_uc, CBK_FILE_VT + o, &stub_zero, 4);
    {
      uint32_t t;
      t = CBK_TRAP_BASE + 0x04u;
      uc_mem_write(g_uc, CBK_FILE_VT + 0x04u, &t, 4); /* release */
      t = CBK_TRAP_BASE + 0x08u;
      uc_mem_write(g_uc, CBK_FILE_VT + 0x08u, &t, 4); /* read */
      t = CBK_TRAP_BASE + 0x0Cu;
      uc_mem_write(g_uc, CBK_FILE_VT + 0x0Cu, &t, 4); /* write */
      t = CBK_TRAP_BASE + 0x10u;
      uc_mem_write(g_uc, CBK_FILE_VT + 0x20u, &t, 4); /* seek */
      t = CBK_TRAP_BASE + 0x14u;
      uc_mem_write(g_uc, CBK_FILE_VT + 0x24u, &t, 4); /* size */
    }
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
  uc_err werr = UC_ERR_OK;
  /* 原实现把每次 uc_write32 的返回值连赋给同一个 err 后只验最后一次，前面的
   * 写入失败会被后面的成功覆盖、被静默吞掉。W(addr,val) 记录首个失败并打日志。 */
#define W(addr, val)                                                          \
  do {                                                                        \
    uc_err _e = uc_write32(g_uc, (addr), (val));                              \
    if (_e != UC_ERR_OK && werr == UC_ERR_OK) {                               \
      werr = _e;                                                              \
      log_error("uc_write32(%s) failed, err: %d\n", #addr, _e);               \
    }                                                                         \
  } while (0)
  // shim 和 tramp 是对射关系,先假设它全部是这样然后后面再做修补修改
  // 假设它全部是函数指针实际上是有对象的后面会进行修补
  // 一个函数指针是四字节所以这里是加四字节
  for (uint32_t i = 0; i < SHIM_SIZE; i += 4) {
    uint32_t addr = SHIM_BASE + i;
    /* 层像素缓冲（LAYER_BUF）必须保持为可自由读写的普通内存，
     * 不能被填成 trap 地址表 —— 否则 applet 通过 GetLayerInfo 拿到的
     * "层缓冲"里全是跳转地址，画面、合成全乱。 */
    if (addr >= LAYER_BUF && addr < LAYER_BUF + LAYER_BUF_SIZE)
      continue;
    /* 解码像素池 / 帧缓冲是数据区，不能填 trap 地址 */
    if (addr >= PIX_POOL && addr < PIX_POOL + PIX_POOL_SIZE)
      continue;
    if (addr >= FRAMEBUF && addr < FRAMEBUF + FRAMEBUF_SIZE)
      continue;
    /* IDisplay 对象同样是数据区：层项的 +0x24 是"该层是否存在"的判断依据
     * （CreateLayer 见非 0 就返回 -8）。填成 trap 值会让 applet 永远建不了层。 */
    if (addr >= DISPLAY && addr < DISPLAY + DISPLAY_OBJ_SIZE)
      continue;
    /* create_cbk 应用上下文（CBK_CTX）同样是数据区：applet 对 +0x8c /
     * +0x90 等字段做"为 0 则创建"的懒初始化，填成 trap 地址会被当成
     * 真实对象解引用（见 emu.h CBK_CTX_SIZE 说明）。 */
    if (addr >= CBK_CTX && addr < CBK_CTX + CBK_CTX_SIZE)
      continue;
    /* ★ 根表 +0x10 槽**留 0**，不要填陷阱地址。
     *
     * 这一槽我们并没有定义（emu_root_traps.h 里 0x04 / 0x10 / 0x18 都是空的 ✗），
     * 于是它被填成"陷阱地址"（0xE50010 之类 ✓），非 0 ✗。而有些 applet 把这个
     * 位置当**对象字段**读 —— 实测 000004dc《仙剑奇侠传》的主循环：
     *
     *   0x28BC  bl #get_root          ; 取根对象
     *   0x28C0  ldr r0,[r0,#0x10]     ; 读 +0x10 当"系统忙 / 暂停"标志
     *   0x28C4  cmp r0,#0
     *   0x28C8  bne 0x28FC            ; ★ 非 0 ⇒ **本帧什么都不做，直接返回**
     *   0x28CC  bl #time64 …          ; 否则才和截止时间比、调 r4->vt[0x18] 干活
     *
     * 结果：每帧都读到 0xE50010 ✗ ⇒ 永远"忙" ⇒ 从不 Update ⇒ **永远白屏** ✗。
     * 【当前状态：实验开关，默认关 ✗】
     * 置 0 之后 000004dc 的主循环**第一次真的跑起来**了 ✓（以前每帧直接 return ✗），
     * 但紧接着撞到第二处用法：`0x24D38 ldr r1,[r1,#0x10]; bx r1`（把它当**函数**
     * 直接调用 ✗）—— 0 当然跳不动 ✗（PC=0 崩）。两处用法矛盾：
     *   0x28C0 读它   → 非 0 ⇒ 本帧什么都不做（"忙"）
     *   0x24D38 调它  → 必须是**可调用地址**
     * ⇒ 这一槽在本族里其实是"**帧回调/处理函数**"，真机上由**框架实现**提供 ✗，
     *   我们还没实现 ⇒ 先保持旧行为（填陷阱地址），等做出那个桩再默认打开 ✓。
     * ZM_ROOT_SLOT10_ZERO=1 可提前体验"主循环真的跑起来"的效果（会崩在 bx ✗）。 */
    if (addr == SHIM_VT_BASE + 0x10 && getenv("ZM_ROOT_SLOT10_ZERO"))
      continue;
    W(addr, TRAMP_BASE + i);
    if (werr != UC_ERR_OK) {
      log_error("shim映射到tramp时出现了错误, err: %d\n", werr);
      return -1;
    }
  }
  /* 层缓冲清零（未绘制区域为黑） */
  {
    static uint8_t zero[4096];
    for (uint32_t off = 0; off < LAYER_BUF_SIZE; off += sizeof(zero)) {
      uint32_t n = LAYER_BUF_SIZE - off;
      if (n > sizeof(zero))
        n = sizeof(zero);
      uc_mem_write(g_uc, LAYER_BUF + off, zero, n);
    }
  }

  /* CBK_CTX 清零：上面已把它排除出 trap 填充，这里确保未使用字段为 0，
   * 让 applet 走"懒创建"分支（+0x8c / +0x90）。 */
  {
    static uint8_t zbuf[256];
    uc_mem_write(g_uc, CBK_CTX, zbuf, CBK_CTX_SIZE);
  }

  /* create_cbk 上下文：CBK_OBJ+0x48 指向 CBK_CTX，
   * 其中 +0x50=IDisplay、+0x54=屏宽、+0x58=屏高（见 emu.h CBK_CTX 说明）。
   * 必须在上面的 SHIM 填充**之后**写，否则会被 trap 地址覆盖。 */
  {
    int sw = 0, sh = 0;
    zm_display_size(&sw, &sh);
    if (!sw || !sh) {
      sw = (int)LAYER_W;
      sh = (int)LAYER_H; /* 显示后端未就绪时退回逻辑分辨率 */
    }
    uc_write32(g_uc, CBK_OBJ + 0x48, CBK_CTX);
    uc_write32(g_uc, CBK_CTX + 0x50, DISPLAY);
    uc_write32(g_uc, CBK_CTX + 0x54, (uint32_t)sw);
    uc_write32(g_uc, CBK_CTX + 0x58, (uint32_t)sh);
    /* 【已试过并撤回，勿盲目重试】+0x5C / +0x60 被 applet 当"引擎给的两个尺寸值"
     * 读（sub_1E78 @0x1EA0-0x1EBC：R2 = ([+0x5C] >= [+0x60]) ? 0x5E : 0x14，
     * 即字号 94 / 20；sub_56C0 还会把这两格拷进 UI 对象 +8 / +0xC）。
     * 2026-09-16 试过填成 (sw, sh)：崩溃现场（pc=0x1EDC / R4=0）与寄存器
     * 完全不变 → 与本次崩溃无关，已撤回。 */
    /* +0x64 = IFileMgr 指针。00000001 在 EV_CREATE（sub_8300）里：
     *   R4 = [[CBK_OBJ+0x48] + 0x64]   // 即 [CBK_CTX + 0x64]
     *   sprintf(buf, "record_flag.dat")
     *   R3 = [R4]->vt[+0x08]; BLX R3   // IFileMgr.OpenFile(fm, path, 1)
     * 这格为 0 时 R4 = NULL → 随后从地址 0（.app 头部）取数据当函数指针 →
     * 启动即崩：err=8 FETCH_UNMAPPED pc=0x91100414 lr=0x835C R4=0，
     * 栈顶还能看到 "record_flag.dat" 那 16 个字节。 */
    uc_write32(g_uc, CBK_CTX + 0x64, G_FileMgr_ADDR);
    log_info("create_cbk 上下文：CBK_OBJ+0x48=0x%X → display=0x%X 屏幕 %dx%d "
             "IFileMgr=0x%X",
             CBK_CTX, DISPLAY, sw, sh, G_FileMgr_ADDR);
  }
  log_info("布局: SHIM_BASE=0x%X TRAMP_BASE=0x%X ROOT_TABLE_ADDR=0x%X G_SHELL_ADDR=0x%X",
           SHIM_BASE, TRAMP_BASE, ROOT_TABLE_ADDR, G_SHELL_ADDR);
  // root
  W(ROOT_TABLE_ADDR, TR_root_getShell);

  // shell（root.getShell 返回；G_SHELL_ADDR 对象 → SHELL_VT_ADDR = g_aee_shell_vtbl）
  W(G_SHELL_ADDR, SHELL_VT_ADDR);
  /* ★ shell 对象 +4 = **内联的工作目录字符串**（参考 ZMAEE_IShell_New：
   *   RootDir = ZMAEE_GetRootDir(); zmaee_strcpy(shell+4, RootDir);
   *   ZMAEE_IShell_GetWorkDir(shell) 就是 `return shell + 4;`）。
   * applet 的路径模板正是 `"%sinfo.dat"` / `"%s%04d.rms"`，那个 %s 读的就是这里 ✗
   * —— 我们以前这里是 0，于是文件名前缀全是脏字节/空，资源永远找不到。
   * 我们的根就是 applet 自己的目录（FileMgr 会自动补 s_data_dir），所以这里给
   * 空串（+ 一个 NUL 兜底）。 */
  {
    const char *workdir = getenv("ZM_WORKDIR") ? getenv("ZM_WORKDIR") : "";
    size_t wl = strlen(workdir);
    if (wl > 200)
      wl = 200;
    uc_mem_write(g_uc, G_SHELL_ADDR + 4, workdir, wl + 1);
  }

  /* FileMgr_VT_ADDR[0x30]：enumFile — sub_82584 枚举 app_list 下文件 */
  // W(FileMgr_VT_ADDR + 0x30, TR_fileMgr_enum);

  // fs
  W(G_FileMgr_ADDR, FileMgr_VT_ADDR);
  W(FILE1, FILE_VT_ADDR);

  // ISetting（0x100000B）/ IMedia 音频（0x100000C）
  W(SETTING, SETTING_VT_ADDR);
  W(G_MEDIA_ADDR, MEDIA_VT_ADDR);

  /* ---- IShell.CreateInstance 返回的服务对象 ----
   * G_NETMGR_ADDR=0x1000004(INetMgr)、G_TAPI_ADDR=0x1000009(ITAPI)。
   * 注意：SVC09/G_TAPI_ADDR 此前漏写对象→虚表指针，applet 拿到后调方法会
   * 读到垃圾函数指针，现已补上。 */
  W(G_NETMGR_ADDR, NETMGR_VT_ADDR);
  W(G_TAPI_ADDR, TAPI_VT_ADDR);

  /* ZMAEE IZip（0x100000F）：返回模拟对象，+0 vtable 指向 ZIP_VT_ADDR，
   * 方法调用经 trap 派发到 zm_zip_stub 观测探针。 */
  W(ZIP_ADDR, ZIP_VT_ADDR);

  /* ---- CBK 回调对象（sub_84E04 返回，vt[+8] 会被 applet 覆写为 sub_82FF8）
   * ---- */
  W(CBK_OBJ, CBK_OBJ_VT_ADDR);
  /* vt[+8] 预写默认实现：applet 随后会覆写；覆写前若被调则走 stub 不崩 */
  W(CBK_OBJ_VT_ADDR + 0x08, TR_cbk_default);

  /* ---- CBK_OBJ+4：applet 的“模块路径”内联 C 串 ----
   * 000004fe 的 fopen 封装 sub_12294 会 str_ctor(dst, create_cbk()+4)，
   * 再把结尾 3 字符覆写成 "zmr" 得到资源名；并要求形如 "X:\..."（否则退回
   * 内嵌资源路径并失败 → sub_B504 返回 null → 崩溃）。
   * 这里写 "<盘符>:\<applet 短名>"，经 fs 的 convert_file_name 归一化后
   * 正好落到数据目录下的 <短名>.zmr。 */
  {
    const char *base = get_filename_from_fullpath(g_app_pathname);
    if (base && base[0]) {
      char modpath[256];
      int n = snprintf(modpath, sizeof(modpath), "c:\\%s", base);
      if (n > 0 && (size_t)n < sizeof(modpath))
        uc_mem_write(g_uc, CBK_OBJ + 4, modpath, (size_t)n + 1);
      log_info("CBK_OBJ+4 模块路径 = \"%s\"", modpath);
    }
  }

  /* ---- stub DLL 对象（loadDLL 返回） ---- */
  W(DLL_OBJ, DLL_OBJ_VT_ADDR);

  /* ---- ZMAEE IDisplay / IBitmap 原生虚表（全局单例 + bitmap 模板）---- */
  W(DISPLAY, DISPLAY_VT_ADDR);
  W(G_BITMAP_ADDR, BITMAP_VT_ADDR);

  /* IDisplay 对象清零：层项 +0x24 必须为 0，applet 的 CreateLayer 才会认为
   * "该层尚不存在"并建层（见 zm_layer.c）。 */
  {
    static uint8_t zb[512];
    for (uint32_t off = 4; off < DISPLAY_OBJ_SIZE; off += sizeof(zb)) {
      uint32_t n = DISPLAY_OBJ_SIZE - off;
      if (n > sizeof(zb))
        n = sizeof(zb);
      uc_mem_write(g_uc, DISPLAY + off, zb, n);
    }
  }

  /* ---- 初始化 IBitmap 单例的字段（RE：ZMAEE_IBitmap_New / _Create）----
   *   +0  vptr      +4  引用计数=1
   *   +8  宽        +12 高
   *   +16 颜色格式  +20 透明色（_Create 初值 -1）
   *   +24 有无调色板 +28 调色板指针  +32 调色板大小
   *   +36 像素指针  +40 调色板大小副本
   * ZMCF 枚举（由 CreateLayerExt 的 a4 与 AlphaBlendRect 的 switch 推出）：
   *   0 = 8bit 索引色(带调色板)、1 = RGB565、2/3/4 = 32bit。
   * 我们提供 RGB565 像素区，故填 1。
   * 以前除 vptr 外全是 build_vtables 填的 trap 值 —— applet 一读
   * GetColorFormat(+16) 就是天文数字，GDI 随之选错分支。 */
  {
    uint32_t bm = 0;
    const char *e = getenv("ZM_BITMAP");
    if (e && e[0])
      bm = (uint32_t)strtoul(e, NULL, 0);
    if (bm) {
      uint32_t one = 1, fmt = 1, neg = 0xFFFFFFFFu, zero = 0;
      uint32_t pix = PIX_POOL;
      W(G_BITMAP_ADDR + 4, one);   /* 引用计数 */
      W(G_BITMAP_ADDR + 8, 0);     /* 宽：0 → blit 不产生内容 */
      W(G_BITMAP_ADDR + 12, 0);    /* 高 */
      W(G_BITMAP_ADDR + 16, fmt);  /* 颜色格式 = RGB565 */
      W(G_BITMAP_ADDR + 20, neg);  /* 透明色 = -1（_Create 初值） */
      W(G_BITMAP_ADDR + 24, (bm >= 2) ? one : zero); /* 调色板标志 */
      W(G_BITMAP_ADDR + 28, zero); /* 调色板指针 */
      W(G_BITMAP_ADDR + 32, zero);
      W(G_BITMAP_ADDR + 36, pix);  /* 像素指针 */
      W(G_BITMAP_ADDR + 40, zero);
    }
    log_info("IBitmap 单例字段：ZM_BITMAP=%u（0=不初始化）", bm);
  }

  /* 解码像素池 / 帧缓冲清零（未加载图片时为黑，而非 trap 垃圾） */
  {
    static uint8_t zbuf[4096];
    for (uint32_t off = 0; off < PIX_POOL_SIZE; off += sizeof(zbuf))
      uc_mem_write(g_uc, PIX_POOL + off, zbuf,
                   (PIX_POOL_SIZE - off) < sizeof(zbuf) ? (PIX_POOL_SIZE - off)
                                                        : sizeof(zbuf));
    for (uint32_t off = 0; off < FRAMEBUF_SIZE; off += sizeof(zbuf))
      uc_mem_write(g_uc, FRAMEBUF + off, zbuf,
                   (FRAMEBUF_SIZE - off) < sizeof(zbuf) ? (FRAMEBUF_SIZE - off)
                                                        : sizeof(zbuf));
  }

  /* 活动层索引。FillRect / DrawBitmap / DrawImage / GetLayerInfo 都用
   *   &v7[13 * v7[2] + 9]        （v7[2] = *(IDisplay+8)）
   * 定位层结构；以前 +8 是 build_vtables 填的 trap 值，applet 自带 GDI
   * 算出的层地址是错的。
   * 【2026-09 修正】以前这里强制写 1（模拟器权宜选择），但真机 `New` 不写
   * 这一格 —— 它在 .bss 里是 **0**，即"启动时活动层 = 层 0"。
   * 这个差异不是首帧问题：applet 在启动阶段就调 SetTransColor / FillRect，
   * 它们都作用于**当时的活动层**，起点是 0 还是 1 决定了这些设置落到哪一层。
   * 现已改回真机值 0。 */
  W(DISPLAY + 8, 0);

  /* ---- 层 0（基础层）：按 RE 的 ZMAEE_IDisplay_New 语义建立 ----
   * 真机上它就是 New 内联构造的（`CreateLayer` 的 `(idx-1) > 0xE` 拒绝 idx=0，
   * 所以层 0 只能由 New 建）。缓冲取自 GetBaseLayerBuffer()，尺寸 =
   * 屏宽×屏高×色深对应字节数，初始 memset(0xFF) 全白。
   * 以前这里是"缺失 + 两处惰性补建"，见 zm_layer_init_base 的说明。 */
  (void)zm_layer_init_base(g_uc, DISPLAY);

  /* ---- 层 1 不再预建（2026-09 判定）----
   * 历史：这里曾"预建层 1"并注释为"实测必须"，否则 GetLayerInfo(1) 返回 -4、
   * applet 缓存到空指针、画面全黑。当时的前提是**层 0 还不存在**（基础层没建），
   * applet 拿不到任何可用缓冲，自然全黑。
   *
   * 现在层 0 已按 RE 的 ZMAEE_IDisplay_New 语义建立，且实测：
   *   - applet **从不调用 CreateLayer**（槽位统计 +0x0C 恒为 0）；
   *   - 它只调 GetLayerInfo(1) 取缓冲，然后 UpdateEx 要求显示 **层 0**。
   * 若真机上"层 1"本就不存在（没人建过），GetLayerInfo(1) 会返回 -4，
   * applet 应当改用它真正会上屏的那一层 —— 这正是我们要验证的。
   * 保留 LAYER_BUF 的定义供其它模块使用，但这里不再往层 1 写载荷。 */

  /* INIT_CTX 显式零填充（Unicorn 默认零，此处双保险，确保 r3+0x100 可读） */
  {
    uint8_t zeros[256] = {0};
    uc_mem_write(g_uc, INIT_CTX, zeros, sizeof(zeros));
  }

#undef W
  if (werr != UC_ERR_OK) {
    log_error("uc_write32/uc_mem_write 失败, err: %d\n", werr);
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
  fclose(fp);
  log_info("blob数据载入完成,文件流已被关闭");

  /*
   * 注意：payload 里那些高 16 位为 0xFFFE/0xFFFF 的 dword（例如
   * IDA 显示的 `off_18D68 DCD loc_188 - 0x18B38`）**不要**做重定位。
   *
   * 它们是 ARM "PC 相对取地址"惯用法的池项：运行时执行
   *     ldr  r0, [pc, #imm]     ; r0 = 该相对偏移
   *     add  r0, pc, r0         ; r0 = 池项位置 + 偏移 = 目标绝对地址
   * 偏移本身就是正确的编码值（向前引用为负 → 补码呈 0xFFFE....），
   * 由 ADD PC 在运行时还原。若把它们改写成绝对地址，ADD PC 会再叠加一次，
   * 导致取到完全错误的地址（实测会把 0x188 变成 0x18EF8 而崩溃）。
   */
  free(buf);
  return 0;
}

int zm_emu_add_hooks() {
  uc_hook hook_code_handle;
  uc_hook hook_unmapped_mem_handle;
  uc_hook hook_shim_mem_handle;
static uc_hook hook_ctx_handle;

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
  /* applet 上下文镜像钩子：默认关闭（实测对 00000502 无改善、且可能引入
   * 误判，见 hook.c 的说明）。需要复现时设 ZM_CTXHOOK=1。 */
  if (!getenv("ZM_NO_CTXHOOK")) {
    err = uc_hook_add(g_uc, &hook_ctx_handle, UC_HOOK_MEM_WRITE,
                      (void *)hook_ctx_mirror, NULL, HEAP_BASE + 0x60u,
                      HEAP_BASE + 0x64u);
    if (err != UC_ERR_OK)
      log_warn("上下文镜像钩子添加失败, err=%d", err);
  }

  log_info("钩子添加完成");
  return 0;
}

/* 指令级追踪（ZM_TRACE=1）：环形缓冲记录最近 TRACE_N 条指令地址。
 * 只在崩溃时输出，用于定位"最后一步跳到了哪里"。 */
/* 环形缓冲大小。48 条对"跳进一大片 0/NOP 区一路滑过去"的崩法不够用
 * ——实测 0000050b 跳进像素池后 48 条全被"滑行"占满，看不到跳飞来源 ✗。
 * 调大到 512（约 2KB 内存，仅 ZM_TRACE=1 时才记录）。 */
#define TRACE_N 4096
uint32_t g_trace[TRACE_N];
int g_trace_pos = 0;
int g_trace_enabled = 0;

static void trace_code_hook(uc_engine *uc, uint64_t address, uint32_t size,
                            void *user) {
  (void)uc;
  (void)size;
  (void)user;
  g_trace[g_trace_pos++ % TRACE_N] = (uint32_t)address;
}

int zm_emu_start_applet() {
  uint32_t stack_ptr = STACK_TOP - STACK_REDZONE;
  uc_reg_write(g_uc, UC_ARM_REG_SP, &stack_ptr);

  uc_reg_write(
      g_uc, UC_ARM_REG_LR,
      &(uint32_t){TR_init_callback}); // LR 是返回地址，这里写入初始化回调
  uc_reg_write(g_uc, UC_ARM_REG_R0, &(uint32_t){SIZE_SLOT});
  uc_reg_write(g_uc, UC_ARM_REG_R1, &(uint32_t){API_SLOT});

  uc_write32(g_uc, BLOB_BASE + ROOT_SLOT_OFF, (uint32_t)ROOT_TABLE_ADDR);

  /* ZM_TRACE=1：开启指令级追踪，崩溃时打印最后 48 条指令地址 */
  {
    const char *t = getenv("ZM_TRACE");
    if (t && t[0] == '1') {
      g_trace_enabled = 1;
      uc_hook hh;
      uc_hook_add(g_uc, &hh, UC_HOOK_CODE, (void *)trace_code_hook, NULL, 1, 0);
      log_info("指令追踪已开启（ZM_TRACE=1）");
    }
  }

  /* ZM_PC=<lo>[,<hi>]：PC 观察点（纯调试）。命中时打印 PC/LR/R0-R3，
   * 用来确认"目标函数被调用时到底拿到什么参数"。详见 hook.h。 */
  {
    const char *p = getenv("ZM_PC");
    if (p && p[0]) {
      char *end = NULL;
      unsigned long lo = strtoul(p, &end, 0);
      unsigned long hi = lo;
      if (end && *end == ',')
        hi = strtoul(end + 1, NULL, 0);
      hook_set_pc_watch((uint32_t)lo, (uint32_t)hi);
    }
  }

  /* ZM_PC2=<lo>[,<hi>]：第二个 PC 观察点（同 ZM_PC，用于同时看两个位置），
   * 例如确认"某对象的构造 +0x10 复制"与"该字段被填"的先后顺序。 */
  {
    const char *p = getenv("ZM_PC2");
    if (p && p[0]) {
      char *end = NULL;
      unsigned long lo = strtoul(p, &end, 0);
      unsigned long hi = lo;
      if (end && *end == ',')
        hi = strtoul(end + 1, NULL, 0);
      hook_set_pc_watch_idx(1, (uint32_t)lo, (uint32_t)hi);
    }
  }

  /* ZM_MW=<lo>,<hi>：内存写监视（纯调试）。命中区间被写时打印 PC/LR/值，
   * 用来查"某字段有没有人写、是谁写的"。详见 hook.h。 */
  {
    const char *p = getenv("ZM_MW");
    if (p && p[0]) {
      char *end = NULL;
      unsigned long lo = strtoul(p, &end, 0);
      unsigned long hi = lo;
      if (end && *end == ',')
        hi = strtoul(end + 1, NULL, 0);
      uc_hook hw;
      uc_err we = uc_hook_add(g_uc, &hw, UC_HOOK_MEM_WRITE,
                              (void *)hook_mem_write_watch, NULL, (uint64_t)lo,
                              (uint64_t)hi);
      log_info("[MW] 监视写区间 0x%lX ~ 0x%lX (err=%d)", lo, hi, we);
    }
  }

  log_info("启动unicorn engine...");
  uc_err e = uc_emu_start(g_uc, APPLET_ENTRY_POINT, STACK_TOP, 0, 0);
  if (e != UC_ERR_OK && g_trace_enabled) {
    log_error("最近指令轨迹（由旧到新）：");
    char tb[512];
    int p = 0;
    for (int i = 0; i < TRACE_N; i++) {
      uint32_t a = g_trace[(g_trace_pos + i) % TRACE_N];
      if (!a)
        continue;
      p += snprintf(tb + p, sizeof(tb) - (size_t)p, "%X ", a);
      if (p > 440) {
        log_error("  %s", tb);
        p = 0;
      }
    }
    if (p)
      log_error("  %s", tb);
  }
  /* 打印停止原因：UC_ERR_OK 表示被 uc_emu_stop 正常停止（或 PC 到达 until），
   * 非 0 则是执行期错误（非法内存访问、未定义指令等），后者需要修。 */
  if (e == UC_ERR_OK) {
    uint32_t pc = 0, sp = 0;
    uc_reg_read(g_uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(g_uc, UC_ARM_REG_SP, &sp);
    log_info("unicorn engine 正常停止（uc_emu_stop），PC=0x%X SP=0x%X", pc, sp);
    /* 正常停止也允许转储（ZM_DUMP_ON_EXIT=1）：专门对付"不崩但不干活"的 applet
     * —— 实测 000004dc 无限跑 1ms 定时器、从不 Update，只有把**运行时的代码** dump
     * 出来（它的内存代码 ≠ .app 文件 ✗）才能看清那个回调在等什么。 */
    if (getenv("ZM_DUMP_ON_EXIT"))
      emu_dump_mem_ranges("正常退出");
  } else {
    /* 出错时 dump 全部通用寄存器 + 栈顶若干字，便于重建调用现场：
     * PC 是出错位置，LR 是最近一次 BL 的返回地址（通常就是"谁跳过去的"），
     * 栈顶若干字能反映 pop {pc} 弹错地址的情况。 */
    static const int regs[] = {UC_ARM_REG_R0,  UC_ARM_REG_R1,  UC_ARM_REG_R2,
                               UC_ARM_REG_R3,  UC_ARM_REG_R4,  UC_ARM_REG_R5,
                               UC_ARM_REG_R6,  UC_ARM_REG_R7,  UC_ARM_REG_R8,
                               UC_ARM_REG_R9,  UC_ARM_REG_R10, UC_ARM_REG_R11,
                               UC_ARM_REG_R12, UC_ARM_REG_SP,  UC_ARM_REG_LR,
                               UC_ARM_REG_PC};
    char buf[512];
    int p = 0;
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
      uint32_t v = 0;
      uc_reg_read(g_uc, regs[i], &v);
      p += snprintf(buf + p, sizeof(buf) - (size_t)p, "R%d=0x%X ", i, v);
      if (i == 12)
        p += snprintf(buf + p, sizeof(buf) - (size_t)p, "| ");
    }
    log_error("unicorn engine 异常停止：err=%d (%s)", e, uc_strerror(e));
    log_error("寄存器：%s", buf);
    /* 解释模式（ARM/Thumb）必须打出来：err=10"非法指令"经常是**模式错位**
     * （同一串字节在另一种模式下解码是合法的）。实测 0000042f 崩在 PC=0x3A48，
     * 那里 ARM 解码是合法的 `str fp,[sp]`。 */
    {
      uint32_t cpsr = 0;
      if (uc_reg_read(g_uc, UC_ARM_REG_CPSR, &cpsr) == UC_ERR_OK)
        log_error("CPSR=0x%X（解释模式：%s）", cpsr,
                  (cpsr & 0x20) ? "Thumb" : "ARM");
    }
    /* 最近 32 次"applet→外部（槽）"调用：崩在野地址/野读时，这一串通常直接
     * 指出"哪个槽的返回值被当指针/尺寸用了"（见 trap.c 的 trap_dump_recent）。 */
    trap_dump_recent();
    /* 最近的指令地址序列：崩在**非陷阱**的野地址时，靠它看清是哪条指令
     * （blx / pop {…,pc} / ldr pc,[rX]）把 PC 带走的（见 hook.c）。 */
    hook_dump_pc_ring();

    /* 崩溃内存转储（见 emu_dump_mem_ranges 的长注释） */
    emu_dump_mem_ranges("崩溃");
    /* PC 处的**实际字节**：err=10 时用来区分三种情况 ——
     *   ① 跳进了数据区（字节与 .app 文件不一致）；
     *   ② 代码被 self-modify / 被别的写入改坏了（同上）；
     *   ③ 字节正常却仍报非法 → 那是状态（CPSR/端序）问题。
     * 对照 .app 文件的原始字节即可判定。 */
    {
      uint32_t pc = 0;
      uint8_t ib[16] = {0};
      uc_reg_read(g_uc, UC_ARM_REG_PC, &pc);
      if (uc_mem_read(g_uc, pc, ib, sizeof(ib)) == UC_ERR_OK) {
        char hex[3 * 16 + 1];
        int q = 0;
        for (unsigned i = 0; i < sizeof(ib) && q + 4 < (int)sizeof(hex); i++)
          q += snprintf(hex + q, sizeof(hex) - (size_t)q, "%02X ", ib[i]);
        log_error("PC=0x%X 处字节=[%s]", pc, hex);
      } else {
        log_error("PC=0x%X 处内存读不出来（未映射？）", pc);
      }
    }

    uint32_t sp = 0;
    uc_reg_read(g_uc, UC_ARM_REG_SP, &sp);
    /* 从 SP-0x20 铺到 SP+0x20：崩在 `pop {…,pc}` 这类"从栈里弹回野地址"时，
     * 真正被弹走的字在 **SP 下方**（pop 之后 SP 已经抬高了）—— 只 dump SP 以上
     * 什么线索也看不到 ✗。实测 000004dc 就靠这一段才看清野地址来自哪个槽。 */
    uint32_t stack[17] = {0};
    if (uc_mem_read(g_uc, sp - 0x20, stack, sizeof(stack)) == UC_ERR_OK) {
      p = 0;
      for (unsigned i = 0; i < 17; i++)
        p += snprintf(buf + p, sizeof(buf) - (size_t)p, "[SP%+d]=0x%X ",
                      (int)((int)i * 4 - 0x20), stack[i]);
      log_error("栈顶：%s", buf);
    }

    /* 关键对象的"虚表链"解析：zmaee 对象首字是虚表指针，
     * 而调用点有"绝对 blx vt[i]"和"相对 add vt[i]+vt"两种风格。
     * 这里把几个可疑寄存器的解引用链打出来，便于判断是哪一种。 */
    static const struct {
      int reg;
      const char *nm;
    } objs[] = {{UC_ARM_REG_R4, "R4"}, {UC_ARM_REG_R3, "R3"},
                {UC_ARM_REG_R2, "R2"}, {UC_ARM_REG_R5, "R5"},
                {UC_ARM_REG_R6, "R6"}, {UC_ARM_REG_R7, "R7"},
                {UC_ARM_REG_R0, "R0"}, {UC_ARM_REG_R1, "R1"},
                {UC_ARM_REG_R8, "R8"}, {UC_ARM_REG_R9, "R9"},
                {UC_ARM_REG_R10, "R10"}, {UC_ARM_REG_LR, "LR"}};
    for (unsigned i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
      uint32_t base = 0;
      uc_reg_read(g_uc, objs[i].reg, &base);
      if (!base)
        continue;
      /* 打 8 个字（[0..0x1C]）：这类"小对象"的字段往往散在前 0x20 字节里，
       * 只打前 4 个会漏掉真正被当指针用的那个（实测 00000462 崩在 [r1+4]）✗。 */
      uint32_t w[8] = {0};
      if (uc_mem_read(g_uc, base, w, sizeof(w)) != UC_ERR_OK)
        continue;
      uint32_t vt = w[0];
      uint32_t vt0 = 0, vt4 = 0, vt18 = 0, vt1c = 0;
      uc_mem_read(g_uc, vt, &vt0, 4);
      uc_mem_read(g_uc, vt + 4, &vt4, 4);
      uc_mem_read(g_uc, vt + 0x18, &vt18, 4);
      uc_mem_read(g_uc, vt + 0x1C, &vt1c, 4);
      log_error("obj@%s=0x%X: [0..0x1C]=%X %X %X %X %X %X %X %X | vt=0x%X "
                "vt[0]=0x%X vt[4]=0x%X vt[0x18]=0x%X vt[0x1C]=0x%X "
                "(vt+vt[4]=0x%X vt+vt[0x1C]=0x%X)",
                objs[i].nm, base, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                vt, vt0, vt4, vt18, vt1c, vt + vt4, vt + vt1c);
      /* this 类对象常把 display / 子对象挂在固定偏移，
       * 打印 this+0x50..0x60 便于确认"display 指针有没有被填上" */
      uint32_t f[6] = {0};
      if (uc_mem_read(g_uc, base + 0x50, f, sizeof(f)) == UC_ERR_OK)
        log_error("   %s+0x50: %X %X %X %X %X %X", objs[i].nm, f[0], f[1], f[2],
                  f[3], f[4], f[5]);
    }
  }
  return 0;
}

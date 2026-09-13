#include "./emu.h"
#include "./hook.h"
#include "./log/log.h"
#include "./tool/uc_helper.h"
#include "./ulibc/ulibc.h"
#include "./zmaee/fs/zm_file_mgr.h"
#include "./zmaee/fs/zm_file.h"
#include "./zmaee/gfx/zm_display.h" /* zm_display_size（初始化 CBK_CTX 用） */
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
uint32_t g_registered_loop = 0;
int g_trap_pause = 0;
int g_disasm = 0; /* 默认关闭；ZM_DISASM=1 打开（会刷大量反汇编日志）*/

csh g_cs_handle;
cs_insn *g_sc_insn;
size_t g_sc_count;
uint8_t g_cscode[16];

/* -------------------- 实现 -------------------- */

int zm_emu_map_memory() {
  uc_err err;
  err = uc_mem_map(g_uc, BLOB_BASE, BLOB_SIZE, UC_PROT_ALL);
  err = uc_mem_map(g_uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  err = uc_mem_map(g_uc, HEAP_BASE, HEAP_SIZE, UC_PROT_ALL);
  err = uc_mem_map(g_uc, SHIM_BASE, SHIM_SIZE, UC_PROT_ALL);
  err = uc_mem_map(g_uc, TRAMP_BASE, TRAMP_SIZE, UC_PROT_ALL);
  if (err != UC_ERR_OK) {
    log_error("uc_mem_map failed, err: %d\n", err);
    return -1;
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
  uc_err err;
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
    err = uc_write32(g_uc, addr, TRAMP_BASE + i);
    if (err != UC_ERR_OK) {
      log_error("shim映射到tramp时出现了错误, err: %d\n", err);
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
    log_info("create_cbk 上下文：CBK_OBJ+0x48=0x%X → display=0x%X 屏幕 %dx%d",
             CBK_CTX, DISPLAY, sw, sh);
  }
  log_info("布局: SHIM_BASE=0x%X TRAMP_BASE=0x%X ROOT_TABLE_ADDR=0x%X G_SHELL_ADDR=0x%X",
           SHIM_BASE, TRAMP_BASE, ROOT_TABLE_ADDR, G_SHELL_ADDR);
  // root
  err = uc_write32(g_uc, ROOT_TABLE_ADDR, TR_root_getShell);

  // shell（root.getShell 返回；G_SHELL_ADDR 对象 → SHELL_VT_ADDR = g_aee_shell_vtbl）
  err = uc_write32(g_uc, G_SHELL_ADDR, SHELL_VT_ADDR);

  /* FileMgr_VT_ADDR[0x30]：enumFile — sub_82584 枚举 app_list 下文件 */
  // err = uc_write32(g_uc, FileMgr_VT_ADDR + 0x30, TR_fileMgr_enum);

  // fs
  err = uc_write32(g_uc, G_FileMgr_ADDR, FileMgr_VT_ADDR);
  err = uc_write32(g_uc, FILE1, FILE_VT_ADDR);

  // ISetting（0x100000B）/ IMedia 音频（0x100000C）
  err = uc_write32(g_uc, SETTING, SETTING_VT_ADDR);
  err = uc_write32(g_uc, G_MEDIA_ADDR, MEDIA_VT_ADDR);

  /* ---- IShell.CreateInstance 返回的服务对象 ----
   * G_NETMGR_ADDR=0x1000004(INetMgr)、G_TAPI_ADDR=0x1000009(ITAPI)。
   * 注意：SVC09/G_TAPI_ADDR 此前漏写对象→虚表指针，applet 拿到后调方法会
   * 读到垃圾函数指针，现已补上。 */
  err = uc_write32(g_uc, G_NETMGR_ADDR, NETMGR_VT_ADDR);
  err = uc_write32(g_uc, G_TAPI_ADDR, TAPI_VT_ADDR);

  /* ZMAEE IZip（0x100000F）：返回模拟对象，+0 vtable 指向 ZIP_VT_ADDR，
   * 方法调用经 trap 派发到 zm_zip_stub 观测探针。 */
  err = uc_write32(g_uc, ZIP_ADDR, ZIP_VT_ADDR);

  /* ---- CBK 回调对象（sub_84E04 返回，vt[+8] 会被 applet 覆写为 sub_82FF8）
   * ---- */
  err = uc_write32(g_uc, CBK_OBJ, CBK_OBJ_VT_ADDR);
  /* vt[+8] 预写默认实现：applet 随后会覆写；覆写前若被调则走 stub 不崩 */
  err = uc_write32(g_uc, CBK_OBJ_VT_ADDR + 0x08, TR_cbk_default);

  /* ---- stub DLL 对象（loadDLL 返回） ---- */
  err = uc_write32(g_uc, DLL_OBJ, DLL_OBJ_VT_ADDR);

  /* ---- ZMAEE IDisplay / IBitmap 原生虚表（全局单例 + bitmap 模板）---- */
  err = uc_write32(g_uc, DISPLAY, DISPLAY_VT_ADDR);
  err = uc_write32(g_uc, BITMAP, BITMAP_VT_ADDR);

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
      err = uc_write32(g_uc, BITMAP + 4, one);   /* 引用计数 */
      err = uc_write32(g_uc, BITMAP + 8, 0);     /* 宽：0 → blit 不产生内容 */
      err = uc_write32(g_uc, BITMAP + 12, 0);    /* 高 */
      err = uc_write32(g_uc, BITMAP + 16, fmt);  /* 颜色格式 = RGB565 */
      err = uc_write32(g_uc, BITMAP + 20, neg);  /* 透明色 = -1（_Create 初值） */
      err = uc_write32(g_uc, BITMAP + 24, (bm >= 2) ? one : zero); /* 调色板标志 */
      err = uc_write32(g_uc, BITMAP + 28, zero); /* 调色板指针 */
      err = uc_write32(g_uc, BITMAP + 32, zero);
      err = uc_write32(g_uc, BITMAP + 36, pix);  /* 像素指针 */
      err = uc_write32(g_uc, BITMAP + 40, zero);
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
   * 算出的层地址是错的。实测 applet 只用层 1（GetLayerInfo 恒请求 r1=1，
   * 且它自己会调 IDisplay.CreateLayer(display, 1, rect, fmt=1)）。
   *
   * 层本身**不再预建**：CreateLayer 才是决定层缓冲与尺寸的地方
   * （固件版会自己 malloc，层已存在则返回 -8），实现见 zm_layer.c。 */
  err = uc_write32(g_uc, DISPLAY + 8, 1);

  /* ---- 预建层 1（实测必须）----
   * applet 在 init 阶段就会 GetLayerInfo 并把返回的**缓冲指针缓存下来**，
   * 之后所有绘制都用那个缓存值。实测：
   *   - 不预建 → 首次 GetLayerInfo 返回 -4，applet 缓存到空指针，
   *     此后永远不往层缓冲写 → 画面全黑（层缓冲统计 100% 为 0）
   *   - 预建   → 首次即拿到 LAYER_BUF，绘制正常（0xFFFF 回到基线 52500）
   * 而 applet 自己随后调用的 CreateLayer(display, 1, rect, fmt=1) 会因为我们
   * 已建该层而返回 -8（固件语义：层已存在），**applet 对此完全能接受**。
   * 层结构由 zm_layer.c 统一定义，这里按同一布局预建。 */
  {
    uint8_t pl[52];
    memset(pl, 0, sizeof(pl));
    uint32_t fmt = 1, x0 = 0, y0 = 0, w = LAYER_W, h = LAYER_H;
    uint32_t buf = LAYER_BUF;
    memcpy(pl + 0x00, &fmt, 4);
    memcpy(pl + 0x04, &x0, 4);
    memcpy(pl + 0x08, &y0, 4);
    memcpy(pl + 0x0C, &w, 4);
    memcpy(pl + 0x10, &h, 4);
    memcpy(pl + 0x1C, &w, 4);
    memcpy(pl + 0x20, &h, 4);
    memcpy(pl + 0x24, &buf, 4);
    uc_mem_write(g_uc, DISPLAY + 36 + 52, pl, sizeof(pl));
  }

  /* INIT_CTX 显式零填充（Unicorn 默认零，此处双保险，确保 r3+0x100 可读） */
  {
    uint8_t zeros[256] = {0};
    err = uc_mem_write(g_uc, INIT_CTX, zeros, sizeof(zeros));
  }

  if (err != UC_ERR_OK) {
    log_error("uc_mem_write failed, err: %d\n", err);
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
  log_info("钩子添加完成");
  return 0;
}

/* 指令级追踪（ZM_TRACE=1）：环形缓冲记录最近 TRACE_N 条指令地址。
 * 只在崩溃时输出，用于定位"最后一步跳到了哪里"。 */
#define TRACE_N 48
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
  uint32_t stack_ptr = STACK_TOP;
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

    uint32_t sp = 0;
    uc_reg_read(g_uc, UC_ARM_REG_SP, &sp);
    uint32_t stack[12] = {0};
    if (uc_mem_read(g_uc, sp, stack, sizeof(stack)) == UC_ERR_OK) {
      p = 0;
      for (unsigned i = 0; i < 12; i++)
        p += snprintf(buf + p, sizeof(buf) - (size_t)p, "[SP+%02u]=0x%X ", i * 4,
                      stack[i]);
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
                {UC_ARM_REG_R6, "R6"}, {UC_ARM_REG_R7, "R7"}};
    for (unsigned i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
      uint32_t base = 0;
      uc_reg_read(g_uc, objs[i].reg, &base);
      if (!base)
        continue;
      uint32_t w[4] = {0};
      if (uc_mem_read(g_uc, base, w, sizeof(w)) != UC_ERR_OK)
        continue;
      uint32_t vt = w[0];
      uint32_t vt0 = 0, vt4 = 0, vt18 = 0, vt1c = 0;
      uc_mem_read(g_uc, vt, &vt0, 4);
      uc_mem_read(g_uc, vt + 4, &vt4, 4);
      uc_mem_read(g_uc, vt + 0x18, &vt18, 4);
      uc_mem_read(g_uc, vt + 0x1C, &vt1c, 4);
      log_error("obj@%s=0x%X: [0..12]=%X %X %X %X | vt=0x%X vt[0]=0x%X "
                "vt[4]=0x%X vt[0x18]=0x%X vt[0x1C]=0x%X (vt+vt[4]=0x%X "
                "vt+vt[0x1C]=0x%X)",
                objs[i].nm, base, w[0], w[1], w[2], w[3], vt, vt0, vt4, vt18,
                vt1c, vt + vt4, vt + vt1c);
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

#include "./emu.h"
#include "./hook.h"
#include "./log/log.h"
#include "./tool/uc_helper.h"
#include "./zmaee/fs/zm_fs.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------- 全局变量定义 -------------------- */
uc_engine *g_uc;
AppletHeader g_header;
uint32_t g_heap_ptr = HEAP_BASE;
uint32_t g_vram_ptr = VRAM_BASE;

uint32_t g_instance = 0;
uint32_t g_handler = 0;
int g_trap_pause = 0;
int g_disasm = 0;

int g_headless = 0;
uint32_t g_hold_ms = 0;
uint32_t g_max_events = 0;
uint32_t g_event_rounds = 0;
uint32_t g_unknown_traps = 0;
int g_stop_requested = 0;

/* 已映射页位图：避免 on-demand 补页时对"已映射页"再次 uc_mem_map
 * （Unicorn 会因重叠返回 UC_ERR_MAP 并向上传播成模拟异常）。 */
#define PAGE_SHIFT 12
#define PAGE_MASK 0xFFFFF000u
static uint8_t *g_page_map = NULL; /* 4GB/4KB = 1M 位 = 128KB */

static void page_mark(uint32_t addr, uint32_t size) {
  uint32_t a = addr & PAGE_MASK;
  uint32_t end = (addr + size + PAGE_MASK) & PAGE_MASK;
  for (; a < end; a += (1u << PAGE_SHIFT)) {
    uint32_t idx = a >> PAGE_SHIFT;
    g_page_map[idx >> 3] |= (uint8_t)(1u << (idx & 7));
  }
}
static int page_is_mapped(uint32_t addr) {
  uint32_t idx = (addr & PAGE_MASK) >> PAGE_SHIFT;
  return (g_page_map[idx >> 3] >> (idx & 7)) & 1;
}

char g_app_pathname[4096] = {0};

csh g_cs_handle;
cs_insn *g_sc_insn;
size_t g_sc_count;
uint8_t g_cscode[16];

/* -------------------- shim 区描述表 -------------------- */

typedef struct {
  uint32_t base;
  const char *name;
  int is_object;   /* 1 = 对象（word0 指向虚表，其余清零）；0 = 函数指针表 */
  uint32_t vtable; /* is_object 时 word0 写入的虚表地址 */
} ShimRegion;

static const ShimRegion k_regions[] = {
    {ROOT, "ROOT", 0, 0},
    {RUNTIME, "RUNTIME", 1, RT_VT},
    {RT_VT, "RT_VT", 0, 0},
    {GFX, "GFX", 1, GFX_VT},
    {GFX_VT, "GFX_VT", 0, 0},
    {FileMgr, "FileMgr", 1, FileMgr_VT},
    {FileMgr_VT, "FileMgr_VT", 0, 0},
    {FILE1, "FILE", 1, FILE_VT},
    {FILE_VT, "FILE_VT", 0, 0},
    {AUDIO, "AUDIO", 1, AUDIO_VT},
    {AUDIO_VT, "AUDIO_VT", 0, 0},
    {AP, "AP", 1, AP_VT},
    {AP_VT, "AP_VT", 0, 0},
    {SVC04, "SVC04", 1, SVC04_VT},
    {SVC04_VT, "SVC04_VT", 0, 0},
    {SVC09, "SVC09", 1, SVC09_VT},
    {SVC09_VT, "SVC09_VT", 0, 0},
    {CBK_OBJ, "CBK", 1, CBK_OBJ_VT},
    {CBK_OBJ_VT, "CBK_VT", 0, 0},
    {DLL_OBJ, "DLL", 1, DLL_OBJ_VT},
    {DLL_OBJ_VT, "DLL_VT", 0, 0},
    {SVC_GENERIC, "SVCG", 1, SVC_GENERIC_VT},
    {SVC_GENERIC_VT, "SVCG_VT", 0, 0},
    {IMAGE_VT, "IMAGE_VT", 0, 0},
};
#define K_REGION_COUNT (sizeof(k_regions) / sizeof(k_regions[0]))

const char *zm_trap_name(uint32_t trap_address) {
  static char buf[64];

  if (trap_address == TR_init_callback)
    return "<init_callback>";
  if (trap_address == TR_enter_event_loop)
    return "<event_loop>";

  if (trap_address < TRAMP_BASE || trap_address >= TRAMP_BASE + TRAMP_SIZE) {
    snprintf(buf, sizeof(buf), "<非陷阱区 0x%08X>", trap_address);
    return buf;
  }

  uint32_t slot = UNTRAP(trap_address);
  for (size_t i = 0; i < K_REGION_COUNT; i++) {
    uint32_t base = k_regions[i].base;
    if (slot >= base && slot < base + ZM_OBJ_STRIDE) {
      snprintf(buf, sizeof(buf), "%s[0x%X]", k_regions[i].name, slot - base);
      return buf;
    }
  }
  if (slot >= ZM_DATA_BASE && slot < ZM_DATA_BASE + ZM_DATA_SIZE) {
    snprintf(buf, sizeof(buf), "DATA[0x%X]", slot - ZM_DATA_BASE);
    return buf;
  }
  snprintf(buf, sizeof(buf), "SHIM+0x%X", slot - SHIM_BASE);
  return buf;
}

/* -------------------- 实现 -------------------- */

int zm_emu_map_memory(void) {
  struct {
    uint32_t base;
    uint32_t size;
    const char *name;
  } regions[] = {
      {BLOB_BASE, BLOB_SIZE, "blob"},
      {STACK_BASE, STACK_SIZE, "stack"},
      {HEAP_BASE, HEAP_SIZE, "heap"},
      {SHIM_BASE, SHIM_SIZE, "shim"},
      {TRAMP_BASE, TRAMP_SIZE, "tramp"},
      {VRAM_BASE, VRAM_SIZE, "vram"},
  };

  if (!g_page_map) {
    g_page_map = calloc(1, (1u << (32 - PAGE_SHIFT)) / 8);
    if (!g_page_map) {
      log_error("分配页位图失败");
      return -1;
    }
  }

  for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
    uc_err err = uc_mem_map(g_uc, regions[i].base, regions[i].size, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
      log_error("uc_mem_map(%s @0x%08X size=0x%X) 失败: %s", regions[i].name,
                regions[i].base, regions[i].size, uc_strerror(err));
      return -1;
    }
    page_mark(regions[i].base, regions[i].size);
    log_debug("映射 %-5s : 0x%08X - 0x%08X", regions[i].name, regions[i].base,
              regions[i].base + regions[i].size);
  }
  log_info("内存映射完成（blob/stack/heap/shim/tramp）");
  return 0;
}

/* 兜底：applet 访问到未映射地址时，按 4KB 页补映射，避免整个模拟直接中止。
 * 注意：绝不能对"已映射页"再次 uc_mem_map —— Unicorn 会因区域重叠返回
 * UC_ERR_MAP，并把它当成致命错误从 uc_emu_start 抛出来。 */
/* 按需补映射的块粒度与上限。
 *
 * 不能按 4KB 逐页映射：QEMU 内部的 section 表有硬上限
 * （exec.c: assert(map->sections_nb < TARGET_PAGE_SIZE)，即 4096 段），
 * applet 一旦拿着野指针连续扫描，几千页就会把 Unicorn abort 掉。
 * 改成 1MB 粒度后段数降低两个数量级，再配一个总量上限兜底。 */
#define ONDEMAND_BLK (1u << 20) /* 1MB */
#define ONDEMAND_MAX_BLK 256    /* 最多补 256MB */

static uint32_t g_ondemand_blks = 0;

bool zm_emu_map_on_demand(uint64_t address) {
  uint64_t page = address & ~(uint64_t)0xFFF;
  if (page > 0xFFFFF000ULL)
    return false;
  if (g_page_map && page_is_mapped((uint32_t)page))
    return true; /* 已映射，直接放行，交给 Unicorn 重试访问 */

  if (g_ondemand_blks >= ONDEMAND_MAX_BLK) {
    log_warn("[MEM] 按需补映射已达上限（%u MB），拒绝 0x%08" PRIx64,
             (unsigned)(ONDEMAND_MAX_BLK), page);
    return false;
  }

  /* 以 1MB 为单位补映射；块内已被占用的部分靠 UC_ERR_MAP 回退到逐页 */
  uint64_t blk = address & ~(uint64_t)(ONDEMAND_BLK - 1);
  uint64_t size = ONDEMAND_BLK;
  if (blk + size > 0x100000000ULL)
    size = 0x100000000ULL - blk;

  uc_err err = uc_mem_map(g_uc, blk, (size_t)size, UC_PROT_ALL);
  if (err == UC_ERR_OK) {
    if (g_page_map)
      page_mark((uint32_t)blk, (uint32_t)size);
    g_ondemand_blks++;
    log_debug("[MEM] 按需补映射 1MB 块 0x%08" PRIx64, blk);
    return true;
  }

  /* 与已有区域重叠：退化成单页映射 */
  err = uc_mem_map(g_uc, page, 0x1000, UC_PROT_ALL);
  if (err == UC_ERR_OK || err == UC_ERR_MAP) {
    if (g_page_map)
      page_mark((uint32_t)page, 0x1000);
    return true;
  }
  log_warn("[MEM] 按需补映射页 0x%08" PRIx64 " 失败: %s", page, uc_strerror(err));
  return false;
}

uint32_t zm_emu_alloc_guest(const void *data, uint32_t len) {
  if (len == 0)
    return 0;
  /* 与 host_malloc 保持同样的 8 字节对齐，否则两者交替分配会把后续
   * 块推到 4-mod-8 地址上，个别 applet 对 malloc 的对齐是有假设的。 */
  uint32_t aligned = (len + 7u) & ~7u;
  if (g_heap_ptr + aligned >= HEAP_END) {
    log_error("客户机堆耗尽（请求 %u 字节）", len);
    return 0;
  }
  uint32_t p = g_heap_ptr;
  g_heap_ptr += aligned;
  if (data)
    uc_mem_write(g_uc, p, data, len);
  return p;
}

uint32_t zm_emu_alloc_vram(const void *data, uint32_t len) {
  if (len == 0)
    return 0;
  uint32_t aligned = (len + 15u) & ~15u;
  if (g_vram_ptr + aligned >= VRAM_END) {
    log_error("显存耗尽（请求 %u 字节，已用 %u KB）", len,
              (g_vram_ptr - VRAM_BASE) / 1024);
    return 0;
  }
  uint32_t p = g_vram_ptr;
  g_vram_ptr += aligned;
  if (data)
    uc_mem_write(g_uc, p, data, len);
  return p;
}

int zm_emu_build_vtables(void) {
  uc_err err;

  /* 1) 先把整个 shim 区填成"槽位 → 同偏移 TRAMP 地址"的函数指针。
   *    这样 applet 取到的任何一个槽位都是合法的陷阱入口，
   *    不会因为漏填某个 vtable 槽而跳到 0 地址。
   *    用宿主机缓冲一次性写入，比逐 4 字节 uc_write32 快两个数量级。 */
  {
    uint32_t *tbl = malloc(SHIM_SIZE);
    if (!tbl) {
      log_error("构建虚表时 malloc(%u) 失败", SHIM_SIZE);
      return -1;
    }
    for (uint32_t i = 0; i < SHIM_SIZE / 4; i++)
      tbl[i] = TRAMP_BASE + i * 4;
    err = uc_mem_write(g_uc, SHIM_BASE, tbl, SHIM_SIZE);
    free(tbl);
    if (err != UC_ERR_OK) {
      log_error("shim 映射到 tramp 失败: %s", uc_strerror(err));
      return -1;
    }
  }

  /* 1.5) 清零"未建模"的 SHIM 区段。
   *   applet 存在两类 vtable 约定：绝对指针调用（读槽→BLX）与
   *   相对偏移调用（target = vt + mem32[vt+slot]）。当相对调用作用到
   *   SHIM 上的对象、或把 SHIM 槽的陷阱指针（巨大绝对地址 0x1072xxxx）
   *   当偏移/字段拼接时（00000001 sub_CFC8、0000050b sub_98BC），
   *   会把陷阱地址当成小偏移 → 野地址（SHIM+0x7FFF8、0x70706132）→
   *   野执行耗尽指令上限。把未建模区清零后，空槽读到 0：
   *   相对调用 target=vt+0、BLX 0 → hook_null_call 安全返回 0；
   *   绝对调用语义等价（这些槽本就不该被调用）。
   *   已建模区（0x00000~0x18000 各对象/vtable、0x40000~0x42000 ZM_DATA）
   *   不受影响。 */
  {
    uint8_t *zeros = calloc(1, SHIM_SIZE);
    if (!zeros) {
      log_error("构建虚表时清零未建模区 calloc 失败");
      return -1;
    }
    /* IMAGE_VT(0x17000) 之后 → ZM_DATA(0x40000) 之前 */
    uc_mem_write(g_uc, SHIM_BASE + 0x18000, zeros, 0x40000 - 0x18000);
    /* ZM_DATA(0x42000 末尾) 之后 → SHIM 末尾 */
    uc_mem_write(g_uc, SHIM_BASE + 0x42000, zeros, SHIM_SIZE - 0x42000);
    free(zeros);
  }

  /* 2) 对象区：word0 = vtable 指针，其余字段清零。
   *    （对象的普通字段若残留 trap 指针，会被 applet 当成巨大的整数用） */
  {
    uint8_t *zeros = calloc(1, ZM_OBJ_STRIDE);
    if (!zeros) {
      log_error("构建虚表时 calloc 失败");
      return -1;
    }
    for (size_t i = 0; i < K_REGION_COUNT; i++) {
      if (!k_regions[i].is_object)
        continue;
      uc_mem_write(g_uc, k_regions[i].base, zeros, ZM_OBJ_STRIDE);
      uc_write32(g_uc, k_regions[i].base, k_regions[i].vtable);
    }
    free(zeros);
  }

  /* 3) 纯数据区清零（SIZE_SLOT / API_SLOT / DUMMY_BUF / INIT_CTX / SCRATCH） */
  {
    uint8_t *zeros = calloc(1, ZM_DATA_SIZE);
    if (!zeros) {
      log_error("构建虚表时 calloc 失败");
      return -1;
    }
    err = uc_mem_write(g_uc, ZM_DATA_BASE, zeros, ZM_DATA_SIZE);
    free(zeros);
    if (err != UC_ERR_OK) {
      log_error("清零数据槽失败: %s", uc_strerror(err));
      return -1;
    }
  }

  log_info("虚表构建完成（%zu 个对象/虚表区）", K_REGION_COUNT);
  return 0;
}

int zm_emu_load_blob(FILE *fp, const long *applet_size) {
  log_info("开始载入 blob 数据");

  if (*applet_size <= 0 || (uint32_t)*applet_size > BLOB_SIZE) {
    log_error("applet 体积 %ld 超出 blob 区容量 0x%X", *applet_size, BLOB_SIZE);
    fclose(fp);
    return -1;
  }

  unsigned char *buf = malloc((size_t)*applet_size);
  if (buf == NULL) {
    log_error("malloc failed");
    fclose(fp);
    return -1;
  }

  size_t bytes_read = fread(buf, 1, (size_t)*applet_size, fp);
  if (bytes_read != (size_t)*applet_size) {
    log_error("读取文件失败，期望 %zu 字节，实际读取 %zu", (size_t)*applet_size,
              bytes_read);
    free(buf);
    fclose(fp);
    return -1;
  }

  uc_err err = uc_mem_write(g_uc, BLOB_BASE, buf, (size_t)*applet_size);
  free(buf);
  fclose(fp);
  if (err != UC_ERR_OK) {
    log_error("uc_mem_write failed: %s", uc_strerror(err));
    return -1;
  }
  log_info("blob 数据载入完成（%ld 字节），文件流已关闭", *applet_size);
  return 0;
}

/* 死循环诊断：在 0x929dc（blx r2，即将调用模块的 vtable+0x40 回调）处挂钩，
 * 打印被调用的回调地址与槽值，定位到底是哪个对象方法在自旋。 */
static void hook_loop_cb(uc_engine *uc, uint64_t address, uint32_t size,
                         void *user_data) {
  static int n = 0;
  if (n >= 64)
    return;
  n++;
  uint32_t r2 = 0, r6 = 0, r8 = 0;
  uc_reg_read(uc, UC_ARM_REG_R2, &r2);
  uc_reg_read(uc, UC_ARM_REG_R6, &r6);
  uc_reg_read(uc, UC_ARM_REG_R8, &r8);
  log_warn("[loop-diag] #%d r8(objA)=0x%08X r6(slot)=0x%08X r2(vt+0x40)=0x%08X",
           n, r8, r6, r2);
}

/* 死循环诊断：首次跳进"数据区"（stack/heap，非 blob/SHIM/TRAMP 代码）执行时，
 * 记录现场与栈回溯，定位是哪个函数指针/虚表方法指向了 heap 数据。 */
static void hook_exec_data(uc_engine *uc, uint64_t address, uint32_t size,
                           void *user_data) {
  static int fired = 0;
  if (fired)
    return;
  fired = 1;
  uint32_t pc = (uint32_t)address, lr = 0, sp = 0;
  uint32_t r[13];
  uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  uc_reg_read(uc, UC_ARM_REG_SP, &sp);
  for (int i = 0; i < 13; i++)
    uc_reg_read(uc, UC_ARM_REG_R0 + i, &r[i]);
  log_warn("[exec-data] 跳进数据区执行! PC=0x%08X LR=0x%08X SP=0x%08X", pc, lr, sp);
  log_warn("  R0..R12 = 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X "
           "0x%08X 0x%08X 0x%08X 0x%08X 0x%08X",
           r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10],
           r[11], r[12]);
  for (int i = 0; i < 10; i++) {
    uint32_t w = uc_read32(uc, sp + i * 4);
    const char *tag = (w >= 0x80000 && w < 0x480000) ? "  <-- 返回blob" : "";
    log_warn("    [SP+0x%X]=0x%08X%s", i * 4, w, tag);
  }
  uint8_t buf[16];
  if (uc_mem_read(uc, pc, buf, 16) == UC_ERR_OK) {
    char hex[64];
    for (int i = 0; i < 16; i++)
      sprintf(hex + i * 3, "%02X ", buf[i]);
    log_warn("  PC处字节: %s", hex);
  }
}

/* 热点计数：ZM_HOT=1 时，对 blob 内每条指令按 16 字节分桶计数，定位自旋循环体。 */
static uint32_t *g_hot = NULL;
static void hook_hot(uc_engine *uc, uint64_t address, uint32_t size, void *ud) {
  uint32_t a = (uint32_t)address;
  if (a >= 0x80000 && a < 0x480000) {
    uint32_t idx = (a - 0x80000) >> 4;
    uint32_t c = ++g_hot[idx];
    (void)c;
  }
}

/* 死循环诊断：进入 blit 外循环条件(0x90bb0)时记录前若干次调用的尺寸，
 * 用于确认清零后精灵宽高是否变成 0（导致黑屏）。 */
static uint32_t g_diag_blit_dst = 0; /* 第一次 blit 的目标地址，用于收尾 dump */
static void hook_blit(uc_engine *uc, uint64_t address, uint32_t size, void *ud) {
  static int n = 0;
  if (n >= 12)
    return;
  uint32_t r0 = 0, r3 = 0, r7 = 0, lr = 0, r2 = 0, r1 = 0, r4 = 0, r6 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  n++;
  if (n == 1)
    g_diag_blit_dst = r1;
  uc_reg_read(uc, UC_ARM_REG_R3, &r3);
  uc_reg_read(uc, UC_ARM_REG_R7, &r7);
  uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  uc_reg_read(uc, UC_ARM_REG_R2, &r2);
  uc_reg_read(uc, UC_ARM_REG_R1, &r1);
  uc_reg_read(uc, UC_ARM_REG_R4, &r4);
  uc_reg_read(uc, UC_ARM_REG_R6, &r6);
  log_warn("[blit#%d] rows(r0)=%u cur(r3)=%u key(r7)=0x%04X "
           "runlen(lr)=%u src=0x%08X dst=0x%08X r4=%u r6=%u",
           n, r0, r3, r7 & 0xFFFF, lr, r2, r1, r4, r6);
  uint8_t buf[16];
  if (uc_mem_read(uc, r2, buf, 16) == UC_ERR_OK) {
    char hex[64];
    for (int i = 0; i < 16; i++)
      sprintf(hex + i * 3, "%02X ", buf[i]);
    log_warn("  src[0:16] = %s", hex);
  }
}

int zm_emu_add_hooks(void) {
  static uc_hook hook_code_handle;
  static uc_hook hook_unmapped_mem_handle;
  static uc_hook hook_shim_mem_handle;
  static uc_hook hook_invalid_insn_handle;

  uc_err err;
  log_info("添加钩子");

  /* 只在 TRAMP 区安装代码钩子即可完成陷阱分发；
   * 需要逐指令反汇编时（-d）才对全地址空间挂钩，避免平时的巨大开销。 */
  if (g_disasm || getenv("ZM_HOOK_ALL")) {
    err = uc_hook_add(g_uc, &hook_code_handle, UC_HOOK_CODE, (void *)hook_code,
                      NULL, 1, 0);
  } else {
    err = uc_hook_add(g_uc, &hook_code_handle, UC_HOOK_CODE, (void *)hook_code,
                      NULL, TRAMP_BASE, TRAMP_BASE + TRAMP_SIZE - 1);
  }
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add(CODE) failed: %s", uc_strerror(err));
    return -1;
  }

  /* 死循环诊断钩子：仅挂钩 0x929dc 单地址 */
  {
    static uc_hook h_loop;
    err = uc_hook_add(g_uc, &h_loop, UC_HOOK_CODE, (void *)hook_loop_cb, NULL,
                     0x929dc, 0x929dc);
    if (err != UC_ERR_OK)
      log_error("uc_hook_add(LOOP_CB) failed: %s", uc_strerror(err));
  }

  /* 死循环诊断钩子：捕获首次跳进数据区（stack/heap）执行 */
  {
    static uc_hook h_exec;
    err = uc_hook_add(g_uc, &h_exec, UC_HOOK_CODE, (void *)hook_exec_data, NULL,
                     0x480000, 0x106A0000 - 1);
    if (err != UC_ERR_OK)
      log_error("uc_hook_add(EXEC_DATA) failed: %s", uc_strerror(err));
  }

  /* 死循环诊断钩子：blob 热点计数（ZM_HOT=1 时） */
  if (getenv("ZM_HOT")) {
    g_hot = calloc((0x480000 - 0x80000) / 16, sizeof(uint32_t));
    static uc_hook h_hot;
    err = uc_hook_add(g_uc, &h_hot, UC_HOOK_CODE, (void *)hook_hot, NULL, 0x80000,
                     0x480000 - 1);
    if (err != UC_ERR_OK)
      log_error("uc_hook_add(HOT) failed: %s", uc_strerror(err));
  }

  /* 死循环诊断钩子：首次进入 blit 外循环(0x90bb0)打印图像尺寸 */
  {
    static uc_hook h_blit;
    err = uc_hook_add(g_uc, &h_blit, UC_HOOK_CODE, (void *)hook_blit, NULL,
                     0x90bb0, 0x90bb0);
    if (err != UC_ERR_OK)
      log_error("uc_hook_add(BLIT) failed: %s", uc_strerror(err));
  }

  err = uc_hook_add(g_uc, &hook_unmapped_mem_handle,
                    UC_HOOK_MEM_UNMAPPED | UC_HOOK_MEM_PROT,
                    (void *)hook_mem_unmapped, NULL, 1, 0);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add(MEM_UNMAPPED) failed: %s", uc_strerror(err));
    return -1;
  }

  err = uc_hook_add(g_uc, &hook_invalid_insn_handle, UC_HOOK_INSN_INVALID,
                    (void *)hook_insn_invalid, NULL, 1, 0);
  if (err != UC_ERR_OK) {
    log_error("uc_hook_add(INSN_INVALID) failed: %s", uc_strerror(err));
    return -1;
  }

  /* 零页执行钩子：捕获空函数指针调用（blx 0 / 未建模的 vtable 子对象），
   * 让它返回 0 而不是直接跑飞。只覆盖 [0, 0x1000)，开销可忽略。 */
  {
    static uc_hook hook_null_call_handle;
    err = uc_hook_add(g_uc, &hook_null_call_handle, UC_HOOK_CODE,
                      (void *)hook_null_call, NULL, 0, 0xFFF);
    if (err != UC_ERR_OK) {
      log_error("uc_hook_add(NULL_CALL) failed: %s", uc_strerror(err));
      return -1;
    }
  }

  /* shim 区读写日志开销很大，只有显式要求（ZM_TRACE_MEM=1）时才挂 */
  if (getenv("ZM_TRACE_MEM")) {
    err = uc_hook_add(g_uc, &hook_shim_mem_handle,
                      UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
                      (void *)hook_shim_mem, NULL, SHIM_BASE,
                      SHIM_BASE + SHIM_SIZE - 1);
    if (err != UC_ERR_OK) {
      log_error("uc_hook_add(SHIM_MEM) failed: %s", uc_strerror(err));
      return -1;
    }
  }

  log_info("钩子添加完成");
  return 0;
}

int zm_emu_start_applet(void) {
  uint32_t stack_ptr = STACK_TOP - 0x100; /* 留一点余量，避免 push 越界 */
  uc_reg_write(g_uc, UC_ARM_REG_SP, &stack_ptr);

  uc_reg_write(g_uc, UC_ARM_REG_LR,
               &(uint32_t){TR_init_callback}); /* 返回地址 = 初始化回调 */
  uc_reg_write(g_uc, UC_ARM_REG_R0, &(uint32_t){SIZE_SLOT});
  uc_reg_write(g_uc, UC_ARM_REG_R1, &(uint32_t){API_SLOT});
  uc_reg_write(g_uc, UC_ARM_REG_R2, &(uint32_t){0});
  uc_reg_write(g_uc, UC_ARM_REG_R3, &(uint32_t){0});

  /* 把外部运行环境的 ROOT 函数表指针写进 applet 的 0x180 槽 */
  uc_write32(g_uc, BLOB_BASE + ROOT_SLOT_OFF, (uint32_t)ROOT);

  log_info("启动 unicorn engine（入口 0x%08X）...", APPLET_ENTRY_POINT);
  /* 注意：until 不能用 0。Unicorn 把 until==0 当成"PC 到达 0 即停止"，
   * 而 applet 的空函数指针调用（blx 0）会让 PC=0，从而被当成正常终止、
   * 我们的空调用兜底层（hook_null_call）根本来不及生效。
   * 这里用一个绝不会被执行的哨兵地址，真正的终止统一由 uc_emu_stop 负责。 */
  /* count 上限：防止个别 applet 陷入原生死循环（框架主循环未正确建模时
   * 会一直空转）把整个批量测试卡死。可用环境变量 ZM_INSN_CAP 调整。 */
  size_t insn_cap = 200 * 1000 * 1000;
  if (getenv("ZM_INSN_CAP")) {
    unsigned long v = strtoul(getenv("ZM_INSN_CAP"), NULL, 0);
    if (v > 0)
      insn_cap = (size_t)v;
  }
  /* 分段执行 + 可选 PC 采样：以 100 万指令为块调用 uc_emu_start，便于在
   * 疑似死循环时定位热点 PC（ZM_SAMPLE=1 时每 500 万指令打印一次 PC）。 */
  bool sampling = getenv("ZM_SAMPLE") != NULL;
  size_t done = 0;
  uint32_t pc = APPLET_ENTRY_POINT;
  uc_err err = UC_ERR_OK;
  while (done < insn_cap && !g_stop_requested) {
    size_t chunk = 1000 * 1000;
    if (done + chunk > insn_cap)
      chunk = insn_cap - done;
    err = uc_emu_start(g_uc, pc, EMU_STOP_SENTINEL, 0, chunk);
    done += chunk;
    if (err != UC_ERR_OK && !g_stop_requested)
      break;
    uc_reg_read(g_uc, UC_ARM_REG_PC, &pc);
    if (sampling && (done % (5 * 1000 * 1000)) == 0)
      log_warn("[sample] done=%zu PC=0x%08X 事件轮数=%u", done, pc,
               g_event_rounds);
  }

  if (err != UC_ERR_OK && !g_stop_requested) {
    uint32_t rpc = 0;
    uc_reg_read(g_uc, UC_ARM_REG_PC, &rpc);
    log_error("模拟异常终止: %s (PC=0x%08X)", uc_strerror(err), rpc);
    return -1;
  }
  if (!g_stop_requested) {
    /* 既不是 uc_emu_stop 主动结束、也没到事件上限——
     * 多半是撞上了指令上限（ZM_INSN_CAP），说明 applet 陷入了未建模的死循环。 */
    log_warn("模拟在指令上限处结束（疑似未建模的死循环，事件轮数=%u, PC=0x%08X, "
             "累计指令=%zu）",
             g_event_rounds, pc, done);
  }
  log_info("模拟正常结束（事件轮数=%u，未实现外部调用=%u）", g_event_rounds,
           g_unknown_traps);

  /* 热点统计输出 */
  if (g_hot) {
    uint32_t nb = (0x480000 - 0x80000) / 16;
    for (int t = 0; t < 10; t++) {
      uint32_t bi = 0, bc = 0;
      for (uint32_t i = 0; i < nb; i++)
        if (g_hot[i] > bc) {
          bc = g_hot[i];
          bi = i;
        }
      if (bc == 0)
        break;
      uint32_t addr = 0x80000 + bi * 16;
      log_warn("[hot] #%d addr=0x%08X count=%u", t + 1, addr, bc);
      g_hot[bi] = 0;
    }
  }

  /* 验证：applet 把精灵像素写到 g_diag_blit_dst（图层缓冲内 +0x344 偏移处），
   * 而合成系统从 L->buf 开头读 → 全黑。dump 该区域确认确有非零像素。 */
  if (getenv("ZM_DUMP_BLIT") && g_diag_blit_dst) {
    uint8_t *tmp = malloc(0x60000);
    if (tmp) {
      if (uc_mem_read(g_uc, g_diag_blit_dst, tmp, 0x60000) == UC_ERR_OK) {
        uint32_t nz = 0;
        for (uint32_t i = 0; i + 1 < 0x60000; i += 2) {
          uint16_t v = (uint16_t)(tmp[i] | (tmp[i + 1] << 8));
          if (v)
            nz++;
        }
        log_warn("[blit-dst] 0x%08X 区域(0x60000B) 非零像素=%u", g_diag_blit_dst,
                 nz);
        char hx[48];
        for (int i = 0; i < 16; i++)
          sprintf(hx + i * 3, "%02X ", tmp[i]);
        log_warn("  前16字节: %s", hx);
      }
      free(tmp);
    }
  }
  return 0;
}

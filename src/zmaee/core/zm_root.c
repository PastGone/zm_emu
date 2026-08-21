#include "zm_root.h"

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../emu.h"
#include "../../log/log.h"
#include "../../tool/odds.h"
#include "../../tool/uc_helper.h"
#include "zm_mem.h" /* host_malloc / host_free */
#include "zm_str.h" /* read_cstr */

/**
 * @brief ROOT[0x60] memset(dst, val, len)
 *
 * sub_841D4 中：MOV R2,#0x214; MOV R1,#0; BL sub_8475C
 * 即 memset(v2, 0, 0x214)。val 取低 8 位按字节填充。
 */
/* 长度合理性检查：applet 里凡是"指针相减"算出来的长度，都可能因为某个
 * 未实现的查找函数返回 0 而变成天文数字。让这类调用直接退化成 no-op，
 * 比把整个客户机内存刷掉要好得多。 */
#define ZM_MAX_BLK (16u * 1024u * 1024u)

static bool len_sane(const char *who, uint32_t len) {
  if (len <= ZM_MAX_BLK)
    return true;
  log_warn("[ROOT] %s 的长度参数不可信（0x%08X），忽略本次调用", who, len);
  return false;
}

uint32_t zm_root_memset(uc_engine *uc, uint32_t dst, uint32_t val,
                        uint32_t len) {
  if (len == 0 || dst == 0)
    return dst;
  if (!len_sane("memset", len))
    return dst;
  uint8_t fill = (uint8_t)(val & 0xFF);
  /* 按块分配填充，避免大 len 时栈溢出 */
  uint32_t chunk = 4096;
  uint8_t *buf = malloc(chunk);
  if (!buf)
    return dst;
  memset(buf, fill, chunk > len ? len : chunk);
  uint32_t off = 0;
  while (off < len) {
    uint32_t n = (len - off < chunk) ? (len - off) : chunk;
    uc_mem_write(uc, dst + off, buf, n);
    off += n;
  }
  free(buf);
  return dst;
}

/* -------- ROOT 表里的标准 C 库槽位 --------
 *
 * 槽位语义由 0000050c 的 thunk 表反推得到：applet 把每个 ROOT 偏移都
 * 包了一层 `ldr rX,[ROOT]; ldr rX,[rX,#OFF]; bx rX` 的桩，而桩的
 * 调用方参数形态直接暴露了函数原型，例如
 *   file 0x25670: `and r1,r1,#0xff; b <ROOT[0x60]>`   → memset
 *   file 0x2567C: `b <ROOT[0x50]>` + 调用处 `cmp r0,#0` → memcmp
 *   file 0x25690: `b <ROOT[0x90]>` + 返回值当长度用     → strlen
 *   file 0x256A4: `mov r2,#10; b <ROOT[0x74]>`          → strtol
 */

/* ROOT[0x5C]：memcpy(dst, src, len) */
uint32_t zm_root_memcpy(uc_engine *uc, uint32_t dst, uint32_t src,
                        uint32_t len) {
  return zm_root_18(uc, dst, len, src);
}

/* ROOT[0x18]：内存复制 (dst, size, src)——参数序与 memcpy 不同。
 * 00000001 sub_16C5C 用它把 8 字节数据复制到栈缓冲，生成存档校验串。 */
uint32_t zm_root_18(uc_engine *uc, uint32_t dst, uint32_t size, uint32_t src) {
  if (!dst || !src || size == 0)
    return dst;
  if (size > ZM_MAX_BLK) {
    log_warn("[ROOT] 0x18 长度参数不可信（0x%X），忽略本次调用", size);
    return dst;
  }
  uint32_t chunk = 4096;
  uint8_t *buf = malloc(chunk < size ? chunk : size);
  if (!buf)
    return dst;
  uint32_t off = 0;
  while (off < size) {
    uint32_t n = (size - off) < chunk ? (size - off) : chunk;
    if (uc_mem_read(uc, src + off, buf, n) != UC_ERR_OK)
      break;
    if (uc_mem_write(uc, dst + off, buf, n) != UC_ERR_OK)
      break;
    off += n;
  }
  free(buf);
  return dst;
}

/* ROOT[0x50]：memcmp(a, b, n) */
uint32_t zm_root_memcmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n) {
  if (n == 0)
    return 0;
  if (!a || !b)
    return (uint32_t)(a == b ? 0 : (a ? 1 : -1));
  if (!len_sane("memcmp", n))
    return 0;
  enum { CHUNK = 1024 };
  uint8_t ba[CHUNK], bb[CHUNK];
  uint32_t off = 0;
  while (off < n) {
    uint32_t k = (n - off < CHUNK) ? (n - off) : CHUNK;
    if (uc_mem_read(uc, a + off, ba, k) != UC_ERR_OK ||
        uc_mem_read(uc, b + off, bb, k) != UC_ERR_OK)
      return 0;
    int r = memcmp(ba, bb, k);
    if (r)
      return (uint32_t)r;
    off += k;
  }
  return 0;
}

/* ROOT[0x58]：memmove(dst, src, len)（与 memcpy 同实现，宿主侧本就分段搬） */
uint32_t zm_root_memmove(uc_engine *uc, uint32_t dst, uint32_t src,
                         uint32_t len) {
  return zm_root_memcpy(uc, dst, src, len);
}

/* ROOT[0x90]：strlen(s) */
uint32_t zm_root_strlen(uc_engine *uc, uint32_t s) {
  if (!s)
    return 0;
  uint32_t n = 0;
  uint8_t c = 0;
  while (n < 64 * 1024) {
    if (uc_mem_read(uc, s + n, &c, 1) != UC_ERR_OK)
      break;
    if (c == 0)
      break;
    n++;
  }
  return n;
}

/* ROOT[0xB0]：strstr(haystack, needle)
 * 405 用 `R2 = strstr(buf,"config") - buf` 求前缀长度。 */
uint32_t zm_root_strstr(uc_engine *uc, uint32_t hay_ptr, uint32_t needle_ptr) {
  if (!hay_ptr || !needle_ptr)
    return 0;
  char hay[2048], needle[256];
  read_cstr(uc, hay_ptr, hay, sizeof(hay));
  read_cstr(uc, needle_ptr, needle, sizeof(needle));
  if (!needle[0])
    return hay_ptr;
  const char *p = strstr(hay, needle);
  if (!p)
    return 0;
  return hay_ptr + (uint32_t)(p - hay);
}

/* ROOT[0x24]：wcstombs(src, src_len, dst, dst_cap)
 *
 * 语义来自 00000440 sub_30414@0x3059C：
 *   ROOT[0x24](str->data, str->len, sp+var_44, 0x40)
 *   随后 strcat(sp+var_44, ".dat") 再拿去 fs.open。
 * 之前没实现，dst 一直是空串，applet 打开的文件名就退化成 ".dat"。
 *
 * ZMAEE 的字符串是 UTF-16，但个别调用点传的是裸 C 串，
 * 这里按内容自动判别（隔字节为 0 视为 UTF-16）。返回写入的字节数。 */
uint32_t zm_root_wcstombs(uc_engine *uc, uint32_t src, uint32_t len,
                          uint32_t dst, uint32_t cap) {
  if (!src || !dst || cap == 0)
    return 0;
  if (len > 4096)
    len = 4096;
  if (cap > 4096)
    cap = 4096;

  uint8_t raw[8192];
  size_t want = (size_t)len * 2;
  if (want > sizeof(raw))
    want = sizeof(raw);
  if (uc_mem_read(uc, src, raw, want) != UC_ERR_OK) {
    /* 读不到 len*2 字节，退一步只读 len 字节当 ASCII */
    want = len;
    if (uc_mem_read(uc, src, raw, want) != UC_ERR_OK)
      return 0;
  }

  /* 判别是否 UTF-16：前若干个奇数字节全为 0 */
  bool utf16 = want >= 2;
  for (size_t i = 1; i < want && i < 16; i += 2) {
    if (raw[i] != 0) {
      utf16 = false;
      break;
    }
  }

  char out[4097];
  uint32_t n = 0;
  if (utf16) {
    for (uint32_t i = 0; i < len && n + 1 < cap; i++) {
      uint16_t wc = (uint16_t)(raw[i * 2] | (raw[i * 2 + 1] << 8));
      if (wc == 0)
        break;
      if (wc < 0x80) {
        out[n++] = (char)wc;
      } else {
        /* 非 ASCII 统一按 '?' 处理，足够拼资源文件名 */
        out[n++] = '?';
      }
    }
  } else {
    for (uint32_t i = 0; i < len && n + 1 < cap; i++) {
      if (raw[i] == 0)
        break;
      out[n++] = (char)raw[i];
    }
  }
  out[n] = '\0';

  uc_mem_write(uc, dst, out, n + 1);
  log_debug("[ROOT] wcstombs(len=%u, utf16=%d) -> \"%s\"", len, (int)utf16, out);
  return n;
}

/* ROOT[0x74]：strtol(nptr, endptr, base) */
uint32_t zm_root_strtol(uc_engine *uc, uint32_t nptr, uint32_t endptr,
                        uint32_t base) {
  if (!nptr)
    return 0;
  char s[128];
  read_cstr(uc, nptr, s, sizeof(s));
  char *end = s;
  long v = strtol(s, &end, (int)base);
  if (endptr)
    uc_write32(uc, endptr, nptr + (uint32_t)(end - s));
  return (uint32_t)v;
}

/* ROOT[0x70]：strtod(nptr, endptr) —— ARM 软浮点按 double 返回 r0:r1，
 * 这里只把整数部分放进 r0，够 applet 判断"能否解析"。 */
uint32_t zm_root_strtod(uc_engine *uc, uint32_t nptr, uint32_t endptr) {
  if (!nptr)
    return 0;
  char s[128];
  read_cstr(uc, nptr, s, sizeof(s));
  char *end = s;
  double v = strtod(s, &end);
  if (endptr)
    uc_write32(uc, endptr, nptr + (uint32_t)(end - s));
  return (uint32_t)(long)v;
}

/**
 * @brief ROOT[0x78] str_assign(str_obj, cstr_ptr)
 *
 * 把 C 字符串赋值给 zmaee 字符串对象（与 zm_strcpy_cstr
 * 写回客户机的布局相同）： +0  : 数据指针（指向 +12 内联缓冲） +4  : 长度 +8  :
 * 容量 +12 : 内联字符串数据（含 '\0'） sub_841D4 中：ADR R1,"app_list"; BL
 * sub_84868。
 */
/* ROOT[0x78]：strcat(dst, src) —— 把 C 串追加到目标缓冲区末尾。
 * 00000440 的 sub_3C2F8 用它把 ".dat" 拼到 wcstombs 输出的路径后面
 * （wcstombs 先写裸 C 串到 var_44，再拼接扩展名）。 */
uint32_t zm_root_str_assign(uc_engine *uc, uint32_t str_obj,
                            uint32_t cstr_ptr) {
  if (str_obj == 0)
    return str_obj;
  char cur[512];
  read_cstr(uc, str_obj, cur, sizeof(cur));
  char add[256];
  read_cstr(uc, cstr_ptr, add, sizeof(add));

  size_t n = strlen(cur);
  size_t m = strlen(add);
  if (n + m + 1 > sizeof(cur))
    m = sizeof(cur) - n - 1;
  memcpy(cur + n, add, m);
  cur[n + m] = '\0';
  uc_mem_write(uc, str_obj, cur, n + m + 1);
  return str_obj;
}

/* ROOT[0x4]：取上下文对象（00000440 sub_3BE8C → ROOT[0x4]）。
 * 调用形态 (r0=sp_buf, r1=0)，返回后 applet 把 ret+0x7080 存入 obj+0xC
 * （sub_30414: ADD R0,R0,#0x4000; ADD R0,R0,#0x3080; STR R0,[R4,#0xC]）。
 * 语义是"返回一个可偏移访问的上下文基址"。这里返回一块静态分配的
 * 大客户机缓冲（0x10000 字节，零填充），保证 +0x7080 落在缓冲内安全访问。 */
static uint32_t s_ctx_base = 0;
uint32_t zm_root_get_ctx(uc_engine *uc, uint32_t sp_buf, uint32_t flag) {
  (void)sp_buf;
  (void)flag;
  if (!s_ctx_base) {
    s_ctx_base = host_malloc(&g_heap_ptr, 0x10000);
    if (s_ctx_base) {
      uint8_t zero[64] = {0};
      for (uint32_t off = 0; off < 0x10000; off += sizeof(zero))
        uc_mem_write(uc, s_ctx_base + off, zero,
                     (0x10000 - off) < sizeof(zero) ? (0x10000 - off)
                                                    : sizeof(zero));
    }
  }
  return s_ctx_base;
}

/* ROOT[0x3C]：调试输出（0000050b sub_2C8CC 是 varargs 打印，
 * 先 vsprintf 到栈缓冲再调 ROOT[0x3C]，r0=格式串、r1=格式化文本）。
 * no-op 返回 0（可选打宿主日志）。 */
uint32_t zm_root_trace(uc_engine *uc, uint32_t fmt, uint32_t text) {
  (void)uc;
  (void)fmt;
  (void)text;
  return 0;
}

/* ROOT[0x144]：状态查询（0000050c sub_3790 → ROOT[0x144]，r0/r1 直通，
 * 返回值与 ctx 状态字 AND 后交回调）。no-op 返回 0（0 & 状态字 = 0）。 */
uint32_t zm_root_get_status(uc_engine *uc, uint32_t a, uint32_t b) {
  (void)uc;
  (void)a;
  (void)b;
  return 0;
}

/* ROOT[0x140]：sub_84C6C(dest, count, maxlen, src_utf16)
 * 00000405 用它在按键/触摸时把 "进入号码"（unk_85E48 UTF-16LE）复制到
 * instance+88 的标签缓冲。src 是 UTF-16LE（2 字节/码元），最多 maxlen 个
 * 码元，写 NUL 终止。count 是 405 传的 SDL_GetTicks() 垃圾值，忽略。 */
uint32_t zm_root_str_utf16_copy(uc_engine *uc, uint32_t dest, uint32_t count,
                                uint32_t maxlen, uint32_t src) {
  (void)count;
  if (!dest || !src)
    return dest;
  if (maxlen > 64)
    maxlen = 64;
  uint16_t buf[65];
  uint32_t n = 0;
  for (; n < maxlen; n++) {
    uint16_t u = 0;
    if (uc_mem_read(uc, src + n * 2, &u, 2) != UC_ERR_OK)
      break;
    buf[n] = u;
    if (u == 0)
      break;
  }
  buf[n] = 0; /* 保证终止 */
  uc_mem_write(uc, dest, buf, (n + 1) * 2);
  return dest;
}

/* ROOT[0x80]：型号查询（00000405 sub_84890 → ROOT[0x80]）。
 * 调用形态 (out_buf, model_str)："Z144"/"A50_GXQ"/"ZA06"/"Z925"/"Z907"/
 * "v69"/"V69" 是**输入查询串**（判断本机型号），返回非 0 走专属布局。
 * 模拟器无真实型号：写默认型号串到 out_buf、返回 0（取默认布局）。 */
uint32_t zm_root_model_check(uc_engine *uc, uint32_t out_buf,
                             uint32_t model_str) {
  (void)uc;
  (void)model_str;
  if (out_buf) {
    const char def[] = "ZMEMU";
    uc_mem_write(uc, out_buf, def, sizeof(def));
  }
  return 0;
}

/* ROOT[0xC8]/[0xD0]：按 key 取系统属性串（00000405 sub_849E4/sub_84A0C）。
 * 调用形态 (buf128B, key)：C8 返回成功标志（0=失败走备用 key 调 D0），
 * D0 返回不检查。模拟器无系统属性：写空串、返 0（C8=失败、D0 无害）。 */
uint32_t zm_root_prop_get(uc_engine *uc, uint32_t buf, uint32_t key) {
  (void)uc;
  (void)key;
  if (buf)
    uc_write32(uc, buf, 0); /* 空串：'\0' */
  return 0;
}

/* ROOT[0x94]：00000001 sub_165DC — 取路径/上下文信息。
 * 在 sub_103D0 中被调用，返回值作为 R2 传给 ROOT[0x98]。
 * stub 返回 0 不影响核心流程（路径缓冲保持空字符串）。 */
uint32_t zm_root_94(uc_engine *uc, uint32_t ctx) {
  (void)uc;
  (void)ctx;
  return 0;
}

/* ROOT[0x98]：00000001 sub_16604(buf, ctx, val) — 写数据目录路径到缓冲。
 * 在 sub_103D0 中调用，将路径写入 0x40 字节的栈缓冲，该缓冲后续传给
 * sub_170BC→sub_17108 用于拼存档文件名 "%s%08x.app"。
 * 缓冲已由调用方置零，stub 不写任何内容（空路径前缀），fs 层会直接用
 * 数据目录作为前缀，行为正确。 */
uint32_t zm_root_98(uc_engine *uc, uint32_t buf, uint32_t ctx, uint32_t val) {
  (void)uc;
  (void)buf;
  (void)ctx;
  (void)val;
  return 0;
}

/* ROOT[0x154]：返回 CBK_OBJ。applet 随后会覆写 CBK_OBJ_VT[+8] 为 sub_82FF8。
 *
 * 注意：CBK_OBJ 是一个"可写虚表"的对象——applet 会把自己的函数地址写进
 * CBK_OBJ_VT[+8]，之后再通过该槽回调自己。由于 CBK_OBJ_VT 位于 SHIM 区
 * 且映射为 UC_PROT_ALL，覆写是允许的；覆写后 applet 跳到的就是自身代码，
 * 不再经过 TRAMP，因此不会命中 handle_trap。 */
uint32_t zm_root_create_cbk(uc_engine *uc) {
  (void)uc;
  /* 0000050b 等 applet 把 ROOT[0x154] 当成 "获取当前 applet 实例/上下文"
   * 的入口（sub_2D060 直接调用它），然后用实例 +0x60 取 runtime、
   * +0x4C 取 GFX。返回 CBK_OBJ 会让这些调用空指针。
   * 若已有实例则返回实例本身，否则退回到可写回调对象。 */
  if (g_instance) {
    /* instance+4 是框架负责的 applet 完整路径字段（applet 用它扫描
     * 反斜杠提取目录，再拼 "<name>.zmr"/"<name>.dat" 等资源文件名；
     * 00000440 的 sub_30414 依赖这个字段带目录，否则拼出 ".dat"）。
     * 个别 applet 初始化时会把它当普通内存覆写掉，导致后面拼出的
     * 文件名残缺（440 拼出 ".dat"）。每次取实例时都恢复一下。 */
    const char *name = zm_app_path_backslash();
    uc_mem_write(uc, g_instance + 4, name, strlen(name) + 1);
    return g_instance;
  }
  return CBK_OBJ;
}

/* 把 g_app_pathname（如 "applet/00000440/00000440.app"）转成
 * "applet\\00000440\\00000440.app"，供 instance+4 使用。
 * applet 扫描最后一个 '\\'(0x5C) 得到目录前缀。 */
const char *zm_app_path_backslash(void) {
  static char buf[4096];
  size_t n = strlen(g_app_pathname);
  if (n >= sizeof(buf))
    n = sizeof(buf) - 1;
  for (size_t i = 0; i < n; i++)
    buf[i] = (g_app_pathname[i] == '/') ? '\\' : g_app_pathname[i];
  buf[n] = '\0';
  return buf;
}

/* ROOT[0xD8]：返回 SDL_GetTicks() 时间戳 */
uint32_t zm_root_get_tick(uc_engine *uc) {
  (void)uc;
  return (uint32_t)SDL_GetTicks();
}

/* 通用 stub：记录命中的 ROOT vtable 偏移与参数，返回 0 */
uint32_t zm_root_stub(uc_engine *uc, uint32_t offset, uint32_t r0, uint32_t r1,
                      uint32_t r2, uint32_t r3) {
  (void)uc;
  log_info("stub root[0x%X] r0=%u r1=%u r2=%u r3=%u", offset, r0, r1, r2, r3);
  return 0;
}

/* CBK 对象 vt[+8] 默认实现（覆写前若被调用） */
uint32_t zm_root_cbk_default(uc_engine *uc, uint32_t r0, uint32_t r1,
                             uint32_t r2, uint32_t r3) {
  (void)uc;
  log_info("stub cbk_default r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}

/* ==================== 定时器（ROOT[0x148] IShell_SetTimer） ====================
 *
 * 真机语义：ISHELL_SetTimer(ms, pfn, param) 注册一个一次性定时器，
 * 到期后向 applet 派发 ZMAEE_EV_TIMER(0x7002) 事件，回调地址由事件携带。
 * 这里只在宿主侧记账：到期时间 + 回调地址 + 参数。
 * 事件循环每轮调用 zm_root_timer_poll() 检查并派发。
 */

#define ZM_MAX_TIMERS 16

typedef struct {
  bool active;
  uint32_t id;      /* 对外返回的 timer_id（= index+1） */
  uint32_t ms;      /* 客户机指定的毫秒数 */
  uint32_t callback; /* 客户机回调地址 */
  uint32_t param;   /* 客户机回调参数 */
  uint32_t ctx;     /* 回调派发时的 R1（仅 00000440 的 sub_30884 需要：
                     * 它把 R1 当"定时器对象"N 解引用，N 从注册现场 R4 捕获） */
  uint64_t deadline_us; /* 宿主单调时钟到期点（微秒） */
  uint64_t virt_deadline_us; /* 虚拟时钟到期点（无头模式，每轮推进） */
} ZmTimerSlot;

static ZmTimerSlot s_timers[ZM_MAX_TIMERS];
static uint32_t s_timer_next_id = 1;

/* 无头模式的虚拟时钟：事件循环每轮推进 ZM_TIMER_VIRT_STEP_US，
 * 使 applet 的 50ms 动画定时器在"宿主时间飞快"的无头测试里也能到期。 */
static uint64_t s_virt_us = 0;
#define ZM_TIMER_VIRT_STEP_US 10000u /* 每轮事件循环 = 虚拟 10ms */

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

void zm_root_timer_tick(void) { s_virt_us += ZM_TIMER_VIRT_STEP_US; }

/* 是否存在活跃定时器（事件循环用它决定是否派发 repaint：
 * 有定时器的 applet 靠定时器驱动动画，每轮硬塞 repaint 反而会把
 * 50ms 定时器反复"重新计时"而永不触发）。 */
bool zm_root_timer_has_active(void) {
  for (int i = 0; i < ZM_MAX_TIMERS; i++)
    if (s_timers[i].active)
      return true;
  return false;
}

uint32_t zm_root_set_timer(uc_engine *uc, uint32_t ms, uint32_t callback,
                           uint32_t param) {
  return zm_root_set_timer_ctx(uc, ms, callback, param, 0);
}

/* 带回调上下文（R1 派发值）的定时器注册。ctx 供 RT_VT[0x3C] 周期定时器使用：
 * 00000440 的 sub_30884 把 R1 当"定时器对象"，注册现场（trap 时）的 R4 即该对象。
 * 仅需 ctx 的 applet（cb==0xB0884）由 trap.c 门控传入，其余传 0 保持原行为。 */
uint32_t zm_root_set_timer_ctx(uc_engine *uc, uint32_t ms, uint32_t callback,
                               uint32_t param, uint32_t ctx) {
  (void)uc;
  if (ms == 0 || callback == 0)
    return 0;
  /* 同一回调重复注册：刷新到期时间（真机为"重新计时"） */
  for (int i = 0; i < ZM_MAX_TIMERS; i++) {
    if (s_timers[i].active && s_timers[i].callback == callback) {
      s_timers[i].ms = ms;
      s_timers[i].param = param;
      s_timers[i].ctx = ctx;
      s_timers[i].deadline_us = now_us() + (uint64_t)ms * 1000ull;
      s_timers[i].virt_deadline_us = s_virt_us + (uint64_t)ms * 1000ull;
      log_debug("timer: 刷新 %ums 回调 0x%X (id=%u)", ms, callback,
                s_timers[i].id);
      return s_timers[i].id;
    }
  }
  for (int i = 0; i < ZM_MAX_TIMERS; i++) {
    if (!s_timers[i].active) {
      s_timers[i].active = true;
      s_timers[i].id = s_timer_next_id++;
      s_timers[i].ms = ms;
      s_timers[i].callback = callback;
      s_timers[i].param = param;
      s_timers[i].ctx = ctx;
      s_timers[i].deadline_us = now_us() + (uint64_t)ms * 1000ull;
      s_timers[i].virt_deadline_us = s_virt_us + (uint64_t)ms * 1000ull;
      log_info("timer: 注册 %ums 回调 0x%X param=0x%X (id=%u)", ms, callback,
               param, s_timers[i].id);
      return s_timers[i].id;
    }
  }
  log_warn("timer: 槽位用尽（%d），忽略 SetTimer(%u, 0x%X)", ZM_MAX_TIMERS, ms,
           callback);
  return 0;
}

uint32_t zm_root_cancel_timer(uc_engine *uc, uint32_t callback) {
  (void)uc;
  for (int i = 0; i < ZM_MAX_TIMERS; i++) {
    if (s_timers[i].active && s_timers[i].callback == callback) {
      s_timers[i].active = false;
      log_info("timer: 取消回调 0x%X (id=%u)", callback, s_timers[i].id);
      return 1;
    }
  }
  return 0;
}

bool zm_root_timer_poll(uc_engine *uc, uint32_t *cb_out, uint32_t *param_out,
                        uint32_t *ctx_out) {
  (void)uc;
  uint64_t now = now_us();
  for (int i = 0; i < ZM_MAX_TIMERS; i++) {
    if (s_timers[i].active && now >= s_timers[i].deadline_us) {
      s_timers[i].active = false; /* 一次性定时器 */
      *cb_out = s_timers[i].callback;
      *param_out = s_timers[i].param;
      if (ctx_out)
        *ctx_out = s_timers[i].ctx;
      log_info("timer: 到期回调 0x%X param=0x%X (id=%u)", s_timers[i].callback,
               s_timers[i].param, s_timers[i].id);
      return true;
    }
  }
  return false;
}

/* 无头模式虚拟时钟版：用 zm_root_timer_tick() 推进的虚拟时间判断到期，
 * 保证 50ms 动画定时器在宿主时间几乎不流逝时也能触发。 */
bool zm_root_timer_poll_virtual(uc_engine *uc, uint32_t *cb_out,
                                uint32_t *param_out, uint32_t *ctx_out) {
  (void)uc;
  for (int i = 0; i < ZM_MAX_TIMERS; i++) {
    if (s_timers[i].active && s_virt_us >= s_timers[i].virt_deadline_us) {
      s_timers[i].active = false; /* 一次性定时器 */
      *cb_out = s_timers[i].callback;
      *param_out = s_timers[i].param;
      if (ctx_out)
        *ctx_out = s_timers[i].ctx;
      log_info("timer: 到期回调 0x%X param=0x%X (id=%u, virt)", s_timers[i].callback,
               s_timers[i].param, s_timers[i].id);
      return true;
    }
  }
  return false;
}

#include "zm_root.h"

#include <SDL2/SDL.h>  /* SDL_GetTicks  (ROOT_TABLE_ADDR[0xD8] get_tick) */
#include <math.h>      /* sqrt/sin/cos  (ROOT_TABLE_ADDR 导入表的双精度数学函数) */
#include <stdlib.h>    /* srand/rand   (ROOT_TABLE_ADDR[0x40]/[0x44]) */
#include <string.h>    /* strlen        (str_assign) */

#include "../../emu.h" /* CBK_OBJ / SHIM_BASE 等地址常量 */
#include "../../log/log.h"
#include "../../tool/uc_helper.h"
#include "zm_str.h" /* read_cstr */

/*
 * zm_root_memset 已删除：TR_root_memset 现接到 ulibc 的 u_memset
 * （见 trap.c），本函数成为死代码。
 */

/**
 * @brief ROOT[0x78] = zmaee_strcat(dst, src) —— 字符串**追加**
 *
 * 【语义定案：是 strcat，不是 strcpy/assign】以前按函数名猜成"赋值"，
 * 用安卓版对照后纠正：
 *   安卓 res/安卓落井下石/0000dc5e.aso.c：
 *     int __fastcall zmold_strcat(int a1, int a2) { return zmaee_strcat(a1, a2); }
 *     zm_qblox_game_data_get()：
 *       memset(buf, 0, 32);
 *       zmold_strcat((int)buf, (int)"e:\\zmol\\zmdata\\qblox.dat");   ← 追加进缓冲
 *       buf[0] = drive;                                              ← 再把首字节换成盘符
 *   手机版 applet 完全同款（就是本槽）：
 *       0x926C  strcat(buf, "e:\\zmol\\zmdata\\qblox.dat")
 *       0x112B0 strcat(out, "zmaee\\data\\")     ← 路径拼装函数里连续追加
 *   两处都要求"追加"，否则路径拼接会互相覆盖。
 *
 * 目标有**两种形态**（实测都有）：
 *   (a) zmaee 字符串对象：+0=data_ptr、+4=len、+8=cap、+12 起内联数据；
 *   (b) **裸 C 串缓冲**：applet 先 memset(buf,0,0x20) 再当 strcat 用（0000042f 的路径缓冲）。
 * 判据：缓冲里已有"像 data_ptr/len 的对象头"才走 (a)。
 *
 * 【为什么必须封顶】目标缓冲大小我们无从得知（裸缓冲是 applet 的栈变量）。
 * 追加超过 250 字节就截断并告警 —— 宁可少拼几个字符，也不能把 applet 的栈写坏。
 */
#define ZM_STRCAT_MAX 250
uint32_t zm_root_str_assign(uc_engine *uc, uint32_t str_obj,
                            uint32_t cstr_ptr) {
  if (str_obj == 0)
    return str_obj;
  char cstr[256];
  read_cstr(uc, cstr_ptr, cstr, sizeof(cstr));
  uint32_t clen = (uint32_t)strlen(cstr);
  if (clen > 255)
    clen = 255;
  uint32_t inline_buf = str_obj + 12;

  /* 【两种目标形态，实测都存在】上面那套"对象布局"只是其中一种：
   *   (a) zmaee 字符串对象：+0=data_ptr、+4=len、+8=cap、+12=内联数据；
   *   (b) **裸 C 串缓冲**：applet 先 `memset(buf, 0, 0x20)` 再调本函数当 strcpy 用。
   *
   * 判据：缓冲里已经有"像 data_ptr/len 的对象头"才走 (a)，否则按裸串写。
   *   - data_ptr == str_obj + 12 （内联布局，我们上次写的就是这个）→ 对象；
   *   - data_ptr 可读且 len < 4096 → 对象（堆/栈上的对象）；
   *   - 其余（首字为 0、或是一串文本、或是野值）→ 裸串。
   *
   * 为什么必须判（0000042f《落井下石》实测 崩溃根因）：它把模板串
   * "e:\zmol\zmdata\qblox.dat" 拼进栈上 32 字节缓冲（先 memset、再 strb 盘符、
   * 再调本函数）后交给 FileMgr::Open。以前无条件按 (a) 写 → 缓冲首字被写成
   * **指针**，文件名于是变成指针的字节（"E\xFB\x17" = 0x17FB45 的小端），
   * 路径全错；紧接着 applet 把这段字符串当对象用，崩在 0x66E8
   * （`ldrne r1,[r0,#0x10]`，r0=0x7461642E=".dat"）。 */
  {
    uint32_t w0 = 0, w1 = 0;
    bool as_obj = false;
    if (uc_mem_read(uc, str_obj, &w0, 4) == UC_ERR_OK &&
        uc_mem_read(uc, str_obj + 4, &w1, 4) == UC_ERR_OK) {
      if (w0 == inline_buf) {
        as_obj = true;
      } else if (w0 != 0 && w1 < 4096) {
        /* ★ 必须**严格**验证"这确实是个字符串对象"。
         *
         * 只判"w0 可读"是不够的：裸缓冲的首字经常**恰好像个合法地址**
         * （payload/堆都在映射区内，低地址尤其容易撞），于是误走对象分支，
         * 把内容写到 w0+len 这个**任意地址**去。
         *
         * 实测代价（0000042f）：路径缓冲首字撞成 0x3A40（payload 里的代码地址），
         * 我们于是把 "zmaee\data\zmdata…" 写进了 applet 的**代码区** 0x3A48，
         * 之后执行到那儿就是"非法指令"（崩溃转储实测：
         *   PC=0x3A48 处字节=[61 65 65 5C 64 61 74 61 …] = "aee\data\zmdata"）。
         * 而且是**间歇性**的 —— 要看首字撞不撞得上，所以格外难查。
         *
         * 现在的判据：data_ptr 可读 + w1<=4096 + (data_ptr+w1) 处是 '\0'
         * + 前 min(w1,32) 字节全是普通可见字符。四者同时成立才当对象。 */
        uint8_t tail = 0xFF;
        if (uc_mem_read(uc, w0 + w1, &tail, 1) == UC_ERR_OK && tail == 0) {
          uint32_t n = w1 > 32 ? 32 : w1;
          char probe[33];
          as_obj = true;
          if (n && uc_mem_read(uc, w0, probe, n) == UC_ERR_OK) {
            for (uint32_t k = 0; k < n; k++) {
              unsigned char ch = (unsigned char)probe[k];
              if (ch < 0x20 || ch >= 0x80) {
                as_obj = false;
                break;
              }
            }
          }
        }
      }
    }
    if (!as_obj) {
      /* 裸 C 串缓冲：**追加**到现有内容之后（= strcat）。
       *
       * ZM_STRASSIGN_COPY=1 是**临时对照开关**：改回"赋值"（覆盖）语义，
       * 用来定位"某个 applet 到底需要 strcat 还是 strcpy"（回归二分用）。 */
      char cur[ZM_STRCAT_MAX + 1];
      cur[0] = '\0';
      read_cstr(uc, str_obj, cur, sizeof(cur));
      uint32_t cur_len = (uint32_t)strlen(cur);
      if (getenv("ZM_STRASSIGN_COPY"))
        cur_len = 0; /* 对照：按覆盖处理 */
      if (cur_len + clen > ZM_STRCAT_MAX) {
        log_warn("strcat: 0x%X 追加后超 %d 字节，截断（现有 %u + 追加 %u）", str_obj,
                 ZM_STRCAT_MAX, cur_len, clen);
        clen = (cur_len < ZM_STRCAT_MAX) ? (ZM_STRCAT_MAX - cur_len) : 0;
      }
      if (clen && uc_mem_write(uc, str_obj + cur_len, cstr, clen) != UC_ERR_OK)
        log_warn("strcat: 裸串追加写 0x%X 失败（%u 字节）", str_obj + cur_len, clen);
      uint8_t zero = 0;
      uc_mem_write(uc, str_obj + cur_len + clen, &zero, 1);
      return str_obj;
    }

    /* 对象形态：也按追加处理（更新 +4/+8 的长度与容量），同样封顶 */
    {
      uint32_t old_len = (w1 <= ZM_STRCAT_MAX) ? w1 : 0;
      if (getenv("ZM_STRASSIGN_COPY"))
        old_len = 0; /* 对照：按覆盖处理 */
      if (old_len + clen > ZM_STRCAT_MAX)
        clen = (old_len < ZM_STRCAT_MAX) ? (ZM_STRCAT_MAX - old_len) : 0;
      if (clen)
        uc_mem_write(uc, w0 + old_len, cstr, clen);
      uint8_t zero = 0;
      uc_mem_write(uc, w0 + old_len + clen, &zero, 1);
      uc_write32(uc, str_obj + 4, old_len + clen); /* 长度 */
      uc_write32(uc, str_obj + 8, old_len + clen); /* 容量 */
      return str_obj;
    }
  }

  /* 兜底（理论上到不了）：按对象布局整体赋值 */
  uc_write32(uc, str_obj, inline_buf); /* 数据指针 */
  uc_write32(uc, str_obj + 4, clen);   /* 长度 */
  uc_write32(uc, str_obj + 8, clen);   /* 容量 */
  uc_mem_write(uc, inline_buf, cstr, clen + 1);
  return str_obj;
}

/* ROOT_TABLE_ADDR[0x154]：返回 CBK_OBJ。applet 随后会覆写 CBK_OBJ_VT_ADDR[+8] 为 sub_82FF8。 */
uint32_t zm_root_create_cbk(uc_engine *uc) {
  (void)uc;
  return CBK_OBJ;
}

/* ROOT_TABLE_ADDR[0x40]/[0x44]：srand / rand。种子只播一次，之后交给 libc。 */
void zm_root_srand(uint32_t seed) {
  static bool seeded = false;
  if (!seeded) {
    srand(seed);
    seeded = true;
  }
}

uint32_t zm_root_rand(void) { return (uint32_t)rand(); }

/* 把 double 结果按 EABI 拆成 r0(低) / r1(高)。r1 需自己写寄存器：
 * trap 框架只回写 R0。 */
static uint32_t ret_double(uc_engine *uc, double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  uint32_t hi = (uint32_t)(bits >> 32);
  uc_reg_write(uc, UC_ARM_REG_R1, &hi);
  return (uint32_t)(bits & 0xFFFFFFFFu);
}

uint32_t zm_root_math(uc_engine *uc, int op, uint32_t lo, uint32_t hi) {
  uint64_t bits = ((uint64_t)hi << 32) | (uint64_t)lo;
  double x;
  memcpy(&x, &bits, sizeof(x));

  switch (op) {
  case ZM_MATH_SQRT:
    return ret_double(uc, sqrt(x));
  case ZM_MATH_COS:
    return ret_double(uc, cos(x));
  case ZM_MATH_SIN:
    return ret_double(uc, sin(x));
  case ZM_MATH_ATAN:
    /* ROOT_TABLE_ADDR[0x110]。依据 applet 调用点（lr=0x17558）：
     *   BL sub_1DE28        （dy/dx 相除）
     *   BL sub_19290        ← 本函数（一元 double）
     *   LDM R11,{R2,R3} + BL sub_1E538   （乘 180/π）
     *   SUB R0, R0, #0x5A   （减 90）
     * 是标准的 atan 求角度写法。 */
    return ret_double(uc, atan(x));
  case ZM_MATH_TAN:
    /* ROOT_TABLE_ADDR[0x11C]。定案依据：applet 只导入 5 个数学函数（由其导入跳板的
     * `LDR R2,[R2,#(loc_XXX - 0x140)]` 模式枚举出槽位），其中
     * 0x104=sqrt / 0x110=atan / 0x114=cos / 0x118=sin 四个槽的实测值把
     * 位置钉死了，剩下的只能是 docs/可能有的函数.md 里
     * 「cos、sin、tan」相邻顺序的第 5 个 = tan。
     * 实测实参 0.7679 / 3.2638 / 2.6005（≈44°/187°/149°）也符合角度分布。 */
    return ret_double(uc, tan(x));
  default:
    return ret_double(uc, 0.0);
  }
}

/* ROOT_TABLE_ADDR[0xD8]：返回 SDL_GetTicks() 时间戳 */
uint32_t zm_root_x68C(uc_engine *uc, uint32_t obj, uint32_t out_buf,
                      uint32_t len, uint32_t self) {
  (void)self;
  if (obj == 0)
    return 0;

  uint32_t head = 0;
  if (uc_mem_read(uc, obj, &head, 4) != UC_ERR_OK)
    return 0;

  /* 把对象的载荷（+0x14 起，若可读）拷进调用方缓冲。
   * 真机这里做的是"惰性加载资源数据"，模拟器没有对应的数据源，
   * 保持缓冲原样、只回填头字段即可保证调用方不读飞。 */
  if (out_buf && len) {
    uint32_t src = obj + 0x14;
    uint8_t probe = 0;
    if (uc_mem_read(uc, src, &probe, 1) == UC_ERR_OK) {
      uint32_t n = len > 0x40 ? 0x40 : len;
      uint8_t tmp[0x40];
      if (uc_mem_read(uc, src, tmp, n) == UC_ERR_OK)
        uc_mem_write(uc, out_buf, tmp, n);
    }
  }
  log_debug("root[0x68C]: obj=0x%X head=%u out=0x%X len=%u", obj, head, out_buf,
            len);
  return head;
}

uint32_t zm_root_get_tick(uc_engine *uc) {
  (void)uc;
  return (uint32_t)SDL_GetTicks();
}

/* zm_root_stub 已删除：零引用，未接线槽位现由 trap.c 的 default 分支
 * 直接 log_error("非法的外部调用") 处理，比通用 stub 定位更精确。 */

/* CBK 对象 vt[+8] 默认实现（覆写前若被调用） */
uint32_t zm_root_cbk_default(uc_engine *uc, uint32_t r0, uint32_t r1,
                             uint32_t r2, uint32_t r3) {
  (void)uc;
  log_info("stub cbk_default r0=%u r1=%u r2=%u r3=%u", r0, r1, r2, r3);
  return 0;
}


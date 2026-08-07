#include "./trap.h"
#include "./log/log.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "./emu.h"
#include "./tool/odds.h"
#include "./zmaee/audio/zm_audio.h"
#include "./zmaee/core/zm_mem.h"
#include "./zmaee/core/zm_root.h"
#include "./zmaee/core/zm_str.h"
#include "./zmaee/fs/zm_fs.h"
#include "./tool/uc_helper.h"
#include "./zmaee/gfx/zm_gfx.h"
#include "./zmaee/gfx/zm_layer.h"
#include "./zmaee/runtime/zm_runtime.h"
#include "event.h"
#include "zmaee/inc/zm_event_code.h"

/* ---------------- 未实现外部调用的兜底 ----------------
 *
 * 设计目标：绝不因为"某个 vtable 槽没实现"而崩溃或阻塞。
 *
 * 策略：
 *   1. 全部 SHIM 槽在启动时已被填成合法的 TRAMP 陷阱地址，
 *      因此任何调用都会落到 handle_trap，而不会跳到 0 地址；
 *   2. 未知槽在这里统一记录（前 N 次打印详细参数，之后只计数），
 *      返回 0（对应 C 语义的 NULL / false / 成功码 0）；
 *   3. 不再调用 pause_console() 阻塞等待人工回车——那会让批量测试挂死。
 */

#define UNKNOWN_LOG_LIMIT 64

/* 记录已经报告过的陷阱地址，避免同一个槽被循环调用时刷屏；
 * 同时统计每个槽的调用次数与首个调用点（LR），便于定位死循环。 */
#define SEEN_CAP 256
static struct {
  uint32_t addr;
  uint32_t count;
  uint32_t first_lr;
} s_seen[SEEN_CAP];
static uint32_t s_seen_n = 0;

/* 返回值：本次是否为该槽的首次出现（首次才打印详细日志） */
static bool mark_seen(uint32_t addr, uint32_t lr) {
  for (uint32_t i = 0; i < s_seen_n; i++) {
    if (s_seen[i].addr == addr) {
      s_seen[i].count++;
      return false; /* 之前报告过 */
    }
  }
  if (s_seen_n < SEEN_CAP) {
    s_seen[s_seen_n].addr = addr;
    s_seen[s_seen_n].count = 1;
    s_seen[s_seen_n].first_lr = lr;
    s_seen_n++;
  }
  return true;
}

void zm_trap_dump_unknown_stats(void) {
  if (s_seen_n == 0)
    return;
  log_info("未实现外部调用分布（共 %u 次 / %u 个槽）：", g_unknown_traps,
           s_seen_n);
  /* 按次数降序打印前 20 个 */
  for (uint32_t k = 0; k < s_seen_n && k < 20; k++) {
    uint32_t best = UINT32_MAX, best_cnt = 0;
    for (uint32_t i = 0; i < s_seen_n; i++) {
      if (s_seen[i].count > best_cnt) {
        best_cnt = s_seen[i].count;
        best = i;
      }
    }
    if (best == UINT32_MAX)
      break;
    log_info("  %-18s x%-9u 首次调用点 LR=0x%08X",
             zm_trap_name(s_seen[best].addr), s_seen[best].count,
             s_seen[best].first_lr);
    s_seen[best].count = 0; /* 已打印，置零以便下一轮选次大 */
  }
}

static uint32_t unknown_trap(uint32_t trap_address, uint32_t r0, uint32_t r1,
                             uint32_t r2, uint32_t r3, uint32_t lr) {
  g_unknown_traps++;
  if (mark_seen(trap_address, lr) && g_unknown_traps <= UNKNOWN_LOG_LIMIT) {
    uint32_t sp = 0, pc = 0;
    if (g_uc) {
      uc_reg_read(g_uc, UC_ARM_REG_SP, &sp);
      uc_reg_read(g_uc, UC_ARM_REG_PC, &pc);
    }
    log_warn("未实现的外部调用 %s (trap=0x%08X) r0=0x%X r1=0x%X r2=0x%X r3=0x%X"
             " sp=0x%X lr=0x%X pc=0x%X → 返回 0",
             zm_trap_name(trap_address), trap_address, r0, r1, r2, r3, sp, lr,
             pc);
  }
  return 0;
}

void handle_trap(uc_engine *uc, uint32_t trap_address) {
  uint32_t r0, r1, r2, r3, sp, lr;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  uc_reg_read(uc, UC_ARM_REG_R1, &r1);
  uc_reg_read(uc, UC_ARM_REG_R2, &r2);
  uc_reg_read(uc, UC_ARM_REG_R3, &r3);
  uc_reg_read(uc, UC_ARM_REG_SP, &sp);
  uc_reg_read(uc, UC_ARM_REG_LR, &lr);

  log_debug("trap %s @0x%08X r0=0x%X r1=0x%X r2=0x%X r3=0x%X sp=0x%X lr=0x%X",
            zm_trap_name(trap_address), trap_address, r0, r1, r2, r3, sp, lr);

  uint32_t ret = 0;

  switch (trap_address) {

  /* ==================== 控制流伪返回地址 ==================== */

  case TR_init_callback: {
    /* applet 入口 sub_188(size_slot, api_slot) 返回后落到这里：
     * 读出 applet 申请的实例大小与事件 handler，分配实例并发 CREATE 事件。 */
    uint32_t size = 0;
    if (uc_mem_read(uc, SIZE_SLOT, &size, 4) != UC_ERR_OK) {
      log_error("读取 SIZE_SLOT 失败");
      g_stop_requested = 1;
      uc_emu_stop(uc);
      return;
    }
    uint32_t handler = 0;
    if (uc_mem_read(uc, API_SLOT + 8, &handler, 4) != UC_ERR_OK) {
      log_error("读取 handler 失败");
      g_stop_requested = 1;
      uc_emu_stop(uc);
      return;
    }

    if (handler == 0) {
      log_error("applet 没有注册事件 handler，无法继续");
      g_stop_requested = 1;
      uc_emu_stop(uc);
      return;
    }
    /* 实例大小做一次合理性钳制，避免个别 applet 写入垃圾值把堆撑爆 */
    if (size == 0 || size > 8u * 1024u * 1024u) {
      log_warn("applet 请求的实例大小异常（%u），钳制为 64KB", size);
      size = 64 * 1024;
    }

    log_info("applet 初始化：instance_size=%u handler=0x%08X", size, handler);

    uint32_t INSTANCE = host_malloc(&g_heap_ptr, size);
    uint8_t *zero_buf = calloc(1, size);
    if (zero_buf) {
      uc_mem_write(uc, INSTANCE, zero_buf, size);
      free(zero_buf);
    }

    /* instance 的 word0 必须指向 applet 自己的虚表（就是 API_SLOT）。
     *
     * 依据：
     *   - 0000050b handler case 1 里执行 (*(*(_DWORD*)a1 + 4))(a1)，
     *     即 instance->vt[4]，而 entry 正是往 api_slot[4] 写了 sub_F7C；
     *   - 00000405 的 sub_84E04()（= ROOT[0x154]，取当前 applet 实例）
     *     之后做 *(*(_DWORD*)v9 + 8) = sub_82FF8，把事件 handler 槽换成
     *     自己的钩子，说明 *instance 就是那张 api 表。
     * 不设这一项会导致大量 "空函数指针调用 @0"。 */
    uc_write32(uc, INSTANCE, API_SLOT);

    /* instance+4 写入带反斜杠的完整路径（如 "applet\00000440\00000440.app"）。
     * 00000440 等 applet 用它在运行时扫描 '\\' 提取目录、拼
     * "<name>.dat"/"<name>.zmr" 等资源文件名——只写短名会让它们拼出
     * 残缺路径（440 拼出 ".dat"）。 */
    const char *name = zm_app_path_backslash();
    uc_mem_write(uc, INSTANCE + 4, name, strlen(name) + 1);
    log_info("instance=0x%08X, vt=0x%08X, 名称=\"%s\"", INSTANCE, API_SLOT,
             name);

    g_instance = INSTANCE;
    g_handler = handler;

    uc_reg_write(uc, UC_ARM_REG_R0, &INSTANCE);
    uint32_t event_code = ZMAEE_EV_CREATE;
    uc_reg_write(uc, UC_ARM_REG_R1, &event_code);
    uint32_t init_type = 0;
    uc_reg_write(uc, UC_ARM_REG_R2, &init_type);
    /* 部分 applet 的 init wrapper 在 a2==0 时解引用 a3[64]（r3+0x100），
     * 传入 INIT_CTX（零填充缓冲）使其可读且判定继续。 */
    uint32_t init_ctx = INIT_CTX;
    uc_reg_write(uc, UC_ARM_REG_R3, &init_ctx);

    uint32_t callback_addr = TR_enter_event_loop;
    uc_reg_write(uc, UC_ARM_REG_LR, &callback_addr);
    uc_reg_write(uc, UC_ARM_REG_PC, &handler);
    return;
  }

  case TR_enter_event_loop: {
    log_debug("命中 TR_enter_event_loop 陷阱（rounds=%u）", g_event_rounds);
    /* applet 的 handler 返回后落到这里：决定继续派发事件还是收尾。
     * 必须 return，绝不能 fall-through 到统一的 "PC=lr" 收尾逻辑。 */
    if (!zm_event_loop_step()) {
      log_info("事件循环结束（共 %u 轮）", g_event_rounds);
      g_stop_requested = 1;
      uc_emu_stop(uc);
    }
    return;
  }

  /* ==================== ROOT 函数表 ==================== */

  case TR_root_queryRuntime:
    ret = RUNTIME;
    break;
  case TR_root_get_ctx:
    /* 00000440 sub_3BE8C：取上下文对象基址 */
    ret = zm_root_get_ctx(uc, r0, r1);
    break;
  case TR_root_malloc:
    ret = host_malloc(&g_heap_ptr, r0);
    break;
  case TR_root_free:
    host_free(r0);
    ret = 0;
    break;
  case TR_root_str_copy:
    ret = zm_strcpy(uc, r0, r1, r2, r3);
    break;
  case TR_root_alloc_big:
    /* 00000440 用 ROOT[0x30](0xC8000) 分配显示面缓冲；复用客户机堆 */
    ret = host_malloc(&g_heap_ptr, r0 ? r0 : 4);
    break;
  case TR_root_free_big:
    host_free(r0);
    ret = 0;
    break;
  case TR_root_get_foo:
    ret = 0;
    break;
  case TR_root_model_check:
    /* 00000405 sub_84890：型号查询，返 0=默认布局 */
    ret = zm_root_model_check(uc, r0, r1);
    break;
  case TR_root_prop_c8:
    /* 00000405 sub_849E4：系统属性查询，返 0=失败走备用 key */
    ret = zm_root_prop_get(uc, r0, r1);
    break;
  case TR_root_prop_d0:
    /* 00000405 sub_84A0C：备用 key 查询，返回不检查 */
    ret = zm_root_prop_get(uc, r0, r1);
    break;
  case TR_root_trace:
    /* 0000050b sub_2C8CC：调试输出，no-op */
    ret = zm_root_trace(uc, r0, r1);
    break;
  case TR_root_wcstombs:
    ret = zm_root_wcstombs(uc, r0, r1, r2, r3);
    break;
  case TR_root_memcmp:
    ret = zm_root_memcmp(uc, r0, r1, r2);
    break;
  case TR_root_memmove:
    ret = zm_root_memmove(uc, r0, r1, r2);
    break;
  case TR_root_memcpy:
    ret = zm_root_memcpy(uc, r0, r1, r2);
    break;
  case TR_root_strlen:
    ret = zm_root_strlen(uc, r0);
    break;
  case TR_root_strstr:
    ret = zm_root_strstr(uc, r0, r1);
    break;
  case TR_root_strtol:
    ret = zm_root_strtol(uc, r0, r1, r2);
    break;
  case TR_root_strtod:
    ret = zm_root_strtod(uc, r0, r1);
    break;
  case TR_root_memset:
    ret = zm_root_memset(uc, r0, r1, r2);
    break;
  case TR_root_sprintf:
    ret = zm_sprintf(uc, r0, r1, r2);
    break;
  case TR_root_str_assign:
    ret = zm_root_str_assign(uc, r0, r1);
    break;
  case TR_root_str_ctor:
    ret = zm_strcpy_cstr(uc, r0, r1);
    break;
  case TR_root_spec_lookup:
    ret = zm_spec_lookup(uc, r0);
    break;
  case TR_root_str_find:
    ret = zm_strchr(uc, r0, r1);
    break;
  case TR_root_get_tick:
    ret = zm_root_get_tick(uc);
    break;
  case TR_root_set_timer:
    ret = zm_root_set_timer(uc, r0, r1, r2);
    break;
  case TR_root_str_utf16:
    /* 00000405 sub_84C6C：复制 UTF-16 串到标签缓冲 */
    ret = zm_root_str_utf16_copy(uc, r0, r1, r2, r3);
    break;
  case TR_root_get_status:
    /* 0000050c sub_3790：状态查询，no-op 返 0 */
    ret = zm_root_get_status(uc, r0, r1);
    break;
  case TR_root_cancel_timer:
    ret = zm_root_cancel_timer(uc, r0);
    break;
  case TR_root_create_cbk:
    ret = zm_root_create_cbk(uc);
    break;

  /* ==================== runtime ==================== */

  case TR_rt_release:
    ret = 0;
    break;
  case TR_rt_queryInterface:
    ret = zm_rt_queryInterface(uc, r1, r2);
    break;
  case TR_rt_getSystemInfo:
    ret = zm_rt_getSystemInfo(uc, r1);
    break;
  case TR_rt_timer:
    /* 周期回调注册 (rt, ms, cb, flags)。00000440 的定时器回调 sub_30884
     * （地址 BLOB+0x30884=0xB0884）把 R1 当"定时器对象"N 解引用，N 是注册
     * 现场（trap 触发时）调用方的 R4（callee-saved 保留）。仅对该回调捕获
     * R4 作 ctx 注入，其他 applet ctx=0 保持原行为（001/50b 不回归）。 */
    {
      uint32_t r4 = 0;
      uc_reg_read(uc, UC_ARM_REG_R4, &r4);
      uint32_t ctx = (r2 == BLOB_BASE + 0x30884U) ? r4 : 0;
      ret = zm_root_set_timer_ctx(uc, r1, r2, r3, ctx);
    }
    break;
  case TR_rt_getter:
    /* 无参 getter，00000440 ×8、00000001 ×1；返回 1 保持"有效对象"语义 */
    ret = 1;
    break;
  case TR_rt_loadDLL:
    ret = zm_rt_loadDLL(uc, r1, r2, r3);
    break;
  case TR_rt_unloadDLL:
    ret = zm_rt_unloadDLL(uc, r1);
    break;
  case TR_rt_loadDLL2:
    ret = zm_rt_loadDLL2(uc, r0, r1, r2, r3);
    break;

  /* ==================== gfx ==================== */

  case TR_gfx_release:
    ret = 0;
    break;

  /* ---- 图层管理（AEE_IDisplay 多图层模型） ----
   * 语义由 00000405 的调试字符串直接坐实：
   *   vt[0x0C] createLayer(id, rect*, flag)
   *   vt[0x20] SetActiveLayer(id)
   *   vt[0x54] clearLayer(id, color)
   *   vt[0x70] fillRect(x, y, w, [sp]h, [sp+4]color)                */
  case TR_gfx_create_layer:
    ret = zm_gfx_create_layer(uc, r1, r2);
    break;
  case TR_gfx_free_layers:
    ret = zm_gfx_free_layers(uc);
    break;
  case TR_gfx_layer_info:
    ret = zm_gfx_layer_info(uc, r1, r2);
    break;
  case TR_gfx_active_layer:
    ret = zm_gfx_active_layer(uc, r1);
    break;
  case TR_gfx_update_layer:
    ret = zm_gfx_update_layer(uc, r1, r2, r3, sp);
    break;
  case TR_gfx_get_active_layer:
    ret = zm_gfx_get_active_layer(uc);
    break;
  case TR_gfx_clear_layer:
    ret = zm_gfx_clear_layer(uc, r1, r2);
    break;
  case TR_gfx_fillRect5C:
    /* 00000440 fillRect(x, y, w, [sp]h)，无颜色参数 */
    ret = zm_gfx_fillRect5C(uc, r1, r2, r3, sp);
    break;
  case TR_gfx_vt44:
    /* 00000405 无参 display 操作，no-op 返 0 */
    ret = zm_gfx_vt44(uc);
    break;
  case TR_gfx_vt84:
    /* 00000440 定时器更新链的绘制辅助(x,y,w/2,...)，no-op 返 0 */
    ret = 0;
    break;
  case TR_gfx_draw_line:
    /* 00000405 DrawLine(x1,y1,x2,[sp]y2,[sp+4]color)，画边框 */
    ret = zm_gfx_draw_line(uc, r1, r2, r3, sp);
    break;
  case TR_gfx_vt8C:
  case TR_gfx_vtB0:
    /* 本轮未触发的未知槽位，stub 返回 0 */
    ret = 0;
    break;
  case TR_gfx_vtB4:
    /* 00000001 三段式横条 blit */
    ret = zm_gfx_vtB4(uc, r0, r1, r2, r3);
    break;
  case TR_gfx_begin_paint:
  case TR_gfx_end_paint:
    ret = 0;
    break;

  /* ---- 图片 ---- */
  case TR_gfx_image_new:
    ret = zm_gfx_image_new(uc, r3 ? r3 : r1);
    break;
  case TR_gfx_image_file:
    /* 00000001 sub_2778 调 GFX_VT[0xA4] 的真实签名是
     *   (this, path, alloc_cb, free_cb, out_ptr@[sp])
     * out_ptr 在第 5 参（栈上）。旧实现把 r2(free 回调)当 out_ptr，
     * 导致图片对象被写到 blob 代码区、applet 的图片对象表保持 0，
     * 后续 getSize/vt[0x94]/vt[0xB4] 全部拿到 0 而全黑。 */
    ret = zm_gfx_image_file(uc, r1, uc_read32(uc, sp));
    break;
  case TR_gfx_draw_image1:
  case TR_gfx_draw_image2:
  case TR_gfx_draw_image98:
  case TR_gfx_draw_image3:
    ret = zm_gfx_draw_image(uc, r1, r2, r3, uc_read32(uc, sp));
    break;

  case TR_img_release:
    ret = zm_img_release(uc, r0);
    break;
  case TR_img_load_file:
    ret = zm_img_load_file(uc, r0, r2, r3);
    break;
  case TR_img_get_size:
    ret = zm_img_get_size(uc, r0, r1);
    break;
  case TR_img_ready:
    ret = 1; /* 同步加载，永远就绪 */
    break;
  case TR_img_make_desc:
    ret = zm_img_make_desc(uc, r0, r1);
    break;

  case TR_gfx_fillRect:
    ret = zm_gfx_fillRect(uc, r1);
    break;
  case TR_gfx_commit:
    ret = zm_gfx_commit(uc);
    break;
  case TR_gfx_get_width:
    ret = zm_gfx_get_width(uc);
    break;
  case TR_gfx_measure_char:
    ret = zm_gfx_measure_char(uc, r0, r1, r2, r3);
    break;
  case TR_gfx_drawText:
    ret = zm_gfx_drawText(uc, r1, r2, r3, sp);
    break;
  case TR_gfx_drawRect:
    ret = zm_gfx_drawRect(uc, r1, r2, r3, sp);
    break;
  case TR_gfx_fillRect2:
    ret = zm_gfx_fillRect2(uc, r1, r2, r3, sp);
    break;

  /* ==================== fs / file ==================== */

  case TR_fileMgr_release:
    ret = zm_fs_release(uc);
    break;
  case TR_fileMgr_open_file:
    /* r0=this(FileMgr), r1=文件名, r2=打开模式 */
    ret = (r0 == 0) ? 0 : zm_fileMgr_open_file(uc, r1, r2);
    break;
  case TR_fileMgr_remove:
    ret = zm_fileMgr_remove(uc, r1);
    break;
  case TR_fileMgr_rename:
    ret = zm_fileMgr_rename(uc, r1, r2);
    break;
  case TR_fileMgr_mkdir:
    ret = zm_fileMgr_mkdir(uc, r1);
    break;
  case TR_fileMgr_rmdir:
    ret = zm_fileMgr_rmdir(uc, r1);
    break;
  case TR_fileMgr_exists:
    ret = zm_fileMgr_exists(uc, r1);
    break;
  case TR_fileMgr_stat:
    ret = zm_fileMgr_stat(uc, r1);
    break;
  case TR_fileMgr_chdir:
    ret = zm_fileMgr_chdir(uc, r1);
    break;
  case TR_fileMgr_enum:
    ret = zm_fileMgr_enum(uc, r1, r2);
    break;

  case TR_file_release:
    ret = zm_file_close(uc, r0);
    break;
  case TR_file_read:
    ret = zm_file_read(uc, r0, r1, r2);
    break;
  case TR_file_write:
    ret = zm_file_write(uc, r0, r1, r2);
    break;
  case TR_file_seek:
    ret = zm_file_seek(uc, r0, r1, r2);
    break;
  case TR_file_size:
    ret = zm_file_size(uc, r0);
    break;
  case TR_file_tell:
    ret = zm_file_tell(uc, r0);
    break;

  /* ==================== audio / ap ==================== */

  case TR_audio_release:
    ret = zm_audio_stop(uc);
    break;
  case TR_audio_stop:
    ret = zm_audio_stop(uc);
    break;
  case TR_audio_vt18:
    /* 00000440 初始化后调 (this, 1) 开启音频，no-op 返 0=成功 */
    ret = zm_audio_set_enable(uc, r1);
    break;
  case TR_audio_vt1C:
    /* 00000001 sub_331C 取播放状态(3 输出)，stub 写 0 使 delay=0 */
    ret = zm_audio_get_play_status(uc, r1, r2, r3);
    break;
  case TR_audio_vt20:
    /* 00000001 传计算出的音频延迟，no-op 返 0=成功 */
    ret = zm_audio_set_delay(uc, r1);
    break;
  case TR_audio_get_status:
    ret = zm_audio_get_status(uc, r1, r2);
    break;
  case TR_ap_release:
    ret = zm_ap_stop(uc);
    break;
  case TR_ap_play:
    ret = zm_ap_play(uc, r2, r3);
    break;
  case TR_ap_stop:
    ret = zm_ap_stop(uc);
    break;

  /* ==================== 服务对象 / DLL / 回调 ==================== */

  case TR_svc04_release:
  case TR_svc09_release:
  case TR_svcg_release:
  case TR_dll_release:
  case TR_cbk_release:
    ret = zm_svc_release(uc);
    break;
  case TR_svc04_x1C:
    ret = zm_svc04_x1C(uc, r0, r1, r2, r3);
    break;
  case TR_svc09_x2C:
    ret = zm_svc09_x2C(uc, r0, r1, r2, r3);
    break;
  case TR_svc09_x40:
    ret = zm_svc09_x40(uc, r0, r1, r2, r3);
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

  /* SHIM 区末尾槽(未建模区最后一个 dword)：00000001/00000506/0000050b/0000050c
   * 都会在对象未初始化时"取到"这个槽并 BLX（sub_CFC8 的相对 vtable 调用）。
   * 返回非 0 避免 applet 把它当"未就绪"无限轮询；0x70706132 野地址由
   * hook_insn_invalid 回退兜底。 */
  case TRAP(SHIM_BASE + 0x7FFF8U):
    ret = 1;
    break;

  /* ==================== 兜底 ==================== */

  default:
    ret = unknown_trap(trap_address, r0, r1, r2, r3, lr);
    break;
  }

  log_debug("  → r0 = 0x%X", ret);
  uc_reg_write(uc, UC_ARM_REG_R0, &ret);
  uc_reg_write(uc, UC_ARM_REG_PC, &lr);

  if (g_trap_pause)
    pause_console();
}

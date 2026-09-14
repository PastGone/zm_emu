#ifndef TEST_DIAG_H
#define TEST_DIAG_H

#include <unicorn/unicorn.h>

/* 调试：ZM_DUMP_BUTTONS=1 时打印 applet 在 init 中计算出的 25 个按钮矩形 */
void zm_diag_dump_buttons(uc_engine *uc);

/* 音频自测：ZM_AUDIO_TEST=N（或 random）播放 .zmr 资源 */
void zm_diag_audio_test(uc_engine *uc);

/* 事件循环：ZM_GFX_HOLD_MS 控制渲染窗口停留毫秒数（0=直到关闭） */
void zm_diag_run_event_loop(void);

#endif
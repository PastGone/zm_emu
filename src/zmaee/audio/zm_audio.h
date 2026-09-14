#ifndef ZM_AUDIO_H
#define ZM_AUDIO_H

// -------------------- ZMAEE IMedia（音频）trap 处理函数声明
// -------------------- 基于 SDL2_mixer 的真实音频播放实现：
//   - 支持 MP3 / WAV / OGG 等（由 SDL_mixer 自动识别）
//   - play 直接从客户机内存读取音频数据并通过 SDL_RWFromMem 播放
//
// 接口归属（RE + 用户确认）：0x100000C = IMedia = 音频，
// 虚表 g_aee_media_vtbl @ .data:0x640E4（25 槽）；play 在 +0x10、
// stop 在 +0x14。旧名 "ap" 是误命名。
#include <stdint.h>
#include <unicorn/unicorn.h>

/* 初始化 SDL_audio + SDL_mixer。成功返回 0，失败返回 -1。 */
int zm_audio_init(void);

/* 关闭 SDL_mixer / SDL_audio 子系统 */
void zm_audio_shutdown(void);

/**
 * @brief IMedia 命令分发器（vtable +0x10，原生 loc_32E80 / sub_32E80）
 *
 * 掌萌 zmapp AEE 媒体层接口实为命令分发器：
 *   int loc_32E80(void *self, int cmd, void *arg1, void *arg2, ...);
 * cmd 取 r1 低 16 位；r2/r3 为前两个参数：
 *   cmd=0x10/0x40 playMusic(String,int)  r2=文件名 r3=整型(常=文件名长度)
 *   cmd=0x41      playSound(String)      r2=文件名
 *   cmd=0x01      playMidSound(裸 MIDI)  cmd=0x02/0x44 playRealSound(裸音频)
 *   cmd=0x42/0x43 loadSound/unloadSound（noop）
 * 文件名模式下从 applet 数据目录读文件播放；否则按裸音频流播放；
 * 未知命令返回 -1（与原生 default 一致）。
 * +0x54（sub_33128）为同一分发器的 thunk，亦调用本函数。
 */
/* 第 5 个参数起在栈上（sp 指向调用方的实参区）——applet 确实会传，
 * 以前被丢掉，见 zm_audio.c 里的说明。 */
uint32_t zm_media_command(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                          uint32_t r3, uint32_t sp);

/* IMedia.stop（+0x14）：停止播放，固定返回 0 */
uint32_t zm_media_stop(uc_engine *uc);

/* IMedia[+0x18] pauseMusic / +0x1C resumeMusic（RE：JNI AEEJNIBridge
 * 同名方法）。 applet 的"声音开关"就是靠这两支真正生效的（见 zm_audio.c
 * 注释）。 */
uint32_t zm_media_pause_music(uc_engine *uc);
uint32_t zm_media_resume_music(uc_engine *uc);

/* ISetting[+0x18]（键 "on"）：非 0 = 声音开、0 = 关（固件字面语义）。
 * 对"用过这个接口的 applet"（如 00000506），在用户**首次点击之前**一律静音
 * —— 满足"进去默认关、点一下开关才出声"。详见 zm_audio.c 的 sound_allowed。 */
void zm_audio_set_sound_flag(uint32_t raw);

/* 用户发生触摸/点击时调用一次：解除"进去默认关"，之后按 app 的开关状态发声。
 * 由 event.c 在派发触摸事件时调用。 */
void zm_audio_note_user_input(void);

/* 完成回调跳板：主循环每轮调用。有排队的回调时写 LR/R0-R2/PC 并返回 true
 * （表示"让 Unicorn 去执行这个 cb"），与 zm_timer_poll 同一套路。
 * RE 依据见 zm_audio.c 的 queue_completion_cb。 */
bool zm_media_pending_cb_poll(uc_engine *uc);

/* IMedia 通用 stub（未实现槽）：记录 offset 与参数，返回 0 */
uint32_t zm_media_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                       uint32_t r2, uint32_t r3);

#endif

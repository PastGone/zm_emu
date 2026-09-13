#ifndef ZM_AUDIO_H
#define ZM_AUDIO_H

// -------------------- ZMAEE IMedia（音频）trap 处理函数声明 --------------------
// 基于 SDL2_mixer 的真实音频播放实现：
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
uint32_t zm_media_command(uc_engine *uc, uint32_t r0, uint32_t r1,
                          uint32_t r2, uint32_t r3);

/* IMedia.stop（+0x14）：停止播放，固定返回 0 */
uint32_t zm_media_stop(uc_engine *uc);

/* IMedia 通用 stub（未实现槽）：记录 offset 与参数，返回 0 */
uint32_t zm_media_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                       uint32_t r2, uint32_t r3);

#endif

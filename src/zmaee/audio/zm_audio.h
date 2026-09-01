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
 * @brief IMedia.play（+0x10）：播放音频
 * @param buf_ptr 音频数据客户机地址（对应 r2）
 * @param buf_len 音频数据长度（对应 r3）
 * @return 固定返回 0
 *
 * @note 从客户机内存读取音频数据，用 Mix_LoadMUST_RW 解码后播放。
 *       上一次播放的资源会被释放。
 */
uint32_t zm_media_play(uc_engine *uc, uint32_t buf_ptr, uint32_t buf_len);

/* IMedia.stop（+0x14）：停止播放，固定返回 0 */
uint32_t zm_media_stop(uc_engine *uc);

/* IMedia 通用 stub（未实现槽）：记录 offset 与参数，返回 0 */
uint32_t zm_media_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                       uint32_t r2, uint32_t r3);

#endif

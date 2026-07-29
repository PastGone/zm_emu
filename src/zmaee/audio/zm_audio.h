#ifndef __ZM_AUDIO_H__
#define __ZM_AUDIO_H__

// -------------------- audio / ap 相关 trap 处理函数声明 --------------------
// 基于 SDL2_mixer 的真实音频播放实现：
//   - 支持 MP3 / WAV / OGG 等（由 SDL_mixer 自动识别）
//   - ap.play 直接从客户机内存读取音频数据并通过 SDL_RWFromMem 播放
#include <stdint.h>
#include <unicorn/unicorn.h>

/* 初始化 SDL_audio + SDL_mixer。成功返回 0，失败返回 -1。 */
int zm_audio_init(void);

/* 关闭 SDL_mixer / SDL_audio 子系统 */
void zm_audio_shutdown(void);

/* audio.stop：停止音频，固定返回 0 */
uint32_t zm_audio_stop(uc_engine *uc);

/**
 * @brief ap.play：播放音频
 * @param buf_ptr 音频数据客户机地址（对应 r2）
 * @param buf_len 音频数据长度（对应 r3）
 * @return 固定返回 0
 *
 * @note 从客户机内存读取音频数据，用 Mix_LoadMUST_RW 解码后播放。
 *       上一次播放的资源会被释放。
 */
uint32_t zm_ap_play(uc_engine *uc, uint32_t buf_ptr, uint32_t buf_len);

/* ap.stop：停止播放，固定返回 0 */
uint32_t zm_ap_stop(uc_engine *uc);

/**
 * @brief AUDIO_VT[0x24] audio.getStatus(this, out4, out_buf)
 *
 * sub_83D44 取 AUDIO 后调 (*AUDIO_VT[0x24])(AUDIO, v8, v7)，返回 0 表成功。
 * applet 读 out_buf[0..2] 与 a1[122..124] 比较，相同则走播放路径。
 * stub：向 out4/out_buf 写 0（使比较命中，因 instance 初值为 0），返 0。
 */
uint32_t zm_audio_get_status(uc_engine *uc, uint32_t out4, uint32_t out_buf);

#endif

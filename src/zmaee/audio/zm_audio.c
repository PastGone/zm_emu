#include "zm_audio.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <stdlib.h>

#include "../../log/log.h"

/* ---------- SDL_mixer 音频后端 ----------
 * ap.play 传入的音频数据一般是 MP3（带 ID3 头），由 SDL_mixer 的
 * Mix_LoadMUST_RW 自动识别格式并解码。
 */

/* 当前正在播放的 music 及其底层数据缓冲。
 * SDL_RWFromMem 不拥有 data，故 data 需在 music 生命周期内保持有效，
 * 下次 play / shutdown 时统一释放。 */
static Mix_Music *g_music = NULL;
static uint8_t *g_music_data = NULL;

static void release_music(void) {
  if (g_music) {
    Mix_HaltMusic();
    Mix_FreeMusic(g_music);
    g_music = NULL;
  }
  if (g_music_data) {
    free(g_music_data);
    g_music_data = NULL;
  }
}

int zm_audio_init(void) {
  /* SDL_INIT_AUDIO 可能已被 gfx 的 SDL_Init 部分初始化，这里幂等叠加 */
  if (SDL_Init(SDL_INIT_AUDIO) != 0) {
    log_error("SDL_Init(AUDIO) failed: %s", SDL_GetError());
    return -1;
  }
  /* 44100Hz, 16bit, 双声道, 4096 字节缓冲 */
  if (Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 4096) != 0) {
    log_error("Mix_OpenAudio failed: %s", Mix_GetError());
    return -1;
  }
  Mix_AllocateChannels(8);
  log_info("zm_audio_init: SDL_mixer 音频后端已就绪");
  return 0;
}

void zm_audio_shutdown(void) {
  release_music();
  Mix_CloseAudio();
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

/* audio.stop：停止音频 */
uint32_t zm_audio_stop(uc_engine *uc) {
  (void)uc;
  release_music();
  Mix_HaltChannel(-1);
  return 0;
}

/**
 * @brief ap.play：播放音频
 *
 * 从客户机地址 buf_ptr 读取 buf_len 字节音频数据，交给 SDL_mixer
 * 解码播放。MP3 / WAV / OGG 等格式可自动识别。
 */
uint32_t zm_ap_play(uc_engine *uc, uint32_t buf_ptr, uint32_t buf_len) {
  if (!buf_ptr || !buf_len)
    return 0;

  /* 释放上一次播放的资源 */
  release_music();

  g_music_data = malloc(buf_len);
  if (!g_music_data) {
    log_error("ap.play: malloc(%u) failed", buf_len);
    return 0;
  }
  uc_mem_read(uc, buf_ptr, g_music_data, buf_len);

  int is_mp3 = (buf_len >= 3 && g_music_data[0] == 'I' &&
                g_music_data[1] == 'D' && g_music_data[2] == '3');
  log_info("  ap.play len=%u mp3=%d", buf_len, is_mp3);

  SDL_RWops *rw = SDL_RWFromMem(g_music_data, (int)buf_len);
  if (!rw) {
    log_error("SDL_RWFromMem failed: %s", SDL_GetError());
    release_music();
    return 0;
  }
  /* freesrc=1：加载后由 SDL_mixer 负责关闭 rw（但不会 free data，
   * data 由 release_music 管理） */
  g_music = Mix_LoadMUS_RW(rw, 1);
  if (!g_music) {
    log_error("Mix_LoadMUS_RW failed: %s", Mix_GetError());
    release_music();
    return 0;
  }
  if (Mix_PlayMusic(g_music, 0) == -1) {
    log_error("Mix_PlayMusic failed: %s", Mix_GetError());
    release_music();
    return 0;
  }
  return 0;
}

/* ap.stop：停止播放 */
uint32_t zm_ap_stop(uc_engine *uc) {
  (void)uc;
  release_music();
  return 0;
}

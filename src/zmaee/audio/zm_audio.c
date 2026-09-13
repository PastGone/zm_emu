#include "zm_audio.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../log/log.h"
#include "../../tool/uc_helper.h"
#include "../fs/zm_file_mgr.h"

/* ---------- SDL_mixer 音频后端（ZMAEE IMedia）----------
 * IMedia.play（+0x10）传入的音频数据一般是 MP3（带 ID3 头），由
 * SDL_mixer 的 Mix_LoadMUS_RW 自动识别格式并解码。
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

/* IMedia 通用 stub（未实现槽） */
uint32_t zm_media_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                       uint32_t r2, uint32_t r3) {
  (void)uc;
  log_info("media stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1,
           r2, r3);
  return 0;
}

/**
 * @brief IMedia.play（+0x10）：播放音频
 *
 * 从客户机地址 buf_ptr 读取 buf_len 字节音频数据，交给 SDL_mixer
 * 解码播放。MP3 / WAV / OGG 等格式可自动识别。
 */
/* 把一段宿主机内存里的音频（MP3/WAV/OGG 等）交给 SDL_mixer 解码播放。
 * 内部自己管理 g_music_data / g_music 全局（先 release 旧的）。成功 0。 */
static int play_from_mem(const uint8_t *data, uint32_t len) {
  if (!data || !len)
    return -1;
  release_music();
  g_music_data = malloc(len);
  if (!g_music_data) {
    log_error("play_from_mem: malloc(%u) failed", len);
    return -1;
  }
  memcpy(g_music_data, data, len);
  SDL_RWops *rw = SDL_RWFromMem(g_music_data, (int)len);
  if (!rw) {
    log_error("SDL_RWFromMem failed: %s", SDL_GetError());
    release_music();
    return -1;
  }
  /* freesrc=1：加载后由 SDL_mixer 负责关闭 rw（但不会 free data，
   * data 由 release_music 管理） */
  g_music = Mix_LoadMUS_RW(rw, 1);
  if (!g_music) {
    log_error("Mix_LoadMUS_RW failed: %s", Mix_GetError());
    release_music();
    return -1;
  }
  if (Mix_PlayMusic(g_music, 0) == -1) {
    log_error("Mix_PlayMusic failed: %s", Mix_GetError());
    release_music();
    return -1;
  }
  return 0;
}

/* 判断缓冲内容是否像一个文件名（可打印 ASCII，且含 '.'，如 sound\bg.mp3）。
 * 裸音频流（MP3/WAV 等）首字节多为 0xFF/'ID3'/'RIFF' 之后即二进制，
 * 不会全部可打印，因此不会误判。 */
static int looks_like_filename(const uint8_t *p, uint32_t len) {
  if (len == 0 || len > 255)
    return 0;
  int has_dot = 0;
  for (uint32_t i = 0; i < len; i++) {
    uint8_t c = p[i];
    if (c == '.')
      has_dot = 1;
    int ok = (c >= 0x20 && c < 0x7F) || c == '\\' || c == '/';
    if (!ok)
      return 0;
  }
  return has_dot;
}

uint32_t zm_media_command(uc_engine *uc, uint32_t r0, uint32_t r1,
                          uint32_t r2, uint32_t r3) {
  (void)r0;
  /* 掌萌 zmapp AEE 媒体层：+0x10（loc_32E80）实为命令分发器，
   * 原型 int loc_32E80(void *self, int cmd, void *arg1, void *arg2, ...)；
   * cmd 取 r1 低 16 位。+0x54（sub_33128）是同一分发器的 thunk，亦走此处。 */
  uint16_t cmd = (uint16_t)(r1 & 0xFFFF);
  log_info("IMedia cmd 分发器: cmd=0x%X  r2=0x%X  r3=0x%X", cmd, r2, r3);

  /* loadSound / unloadSound：预加载/卸载，emu 按需播放，noop */
  if (cmd == 0x42 || cmd == 0x43) {
    log_info("IMedia cmd 0x%X %s (noop)", cmd,
             cmd == 0x42 ? "loadSound" : "unloadSound");
    return 0;
  }

  /* ---- 文件名模式：playMusic(0x10/0x40) / playSound(0x41) ----
   * r2 = 文件名（C 串）。playMusic 以 r3 为长度上界；playSound 按 null 结尾。
   * 读到后反斜杠归一为正斜杠，从 applet 数据目录读出文件播放。 */
  if (cmd == 0x10 || cmd == 0x40 || cmd == 0x41) {
    char name[256];
    uint32_t n = 0;
    if (cmd == 0x41) {
      uint8_t tmp[256];
      if (r2 && uc_mem_read(uc, r2, tmp, sizeof(tmp) - 1) == UC_ERR_OK) {
        for (; n < sizeof(tmp) - 1 && tmp[n]; n++)
          name[n] = (char)tmp[n];
      }
      name[n] = 0;
    } else {
      n = (r3 < sizeof(name) - 1) ? r3 : (sizeof(name) - 1);
      if (r2 && uc_mem_read(uc, r2, (uint8_t *)name, n) == UC_ERR_OK) {
        name[n] = 0;
        for (uint32_t i = 0; i < n && name[i]; i++)
          if (name[i] == '\\')
            name[i] = '/';
      } else {
        n = 0;
        name[0] = 0;
      }
    }
    if (n && looks_like_filename((const uint8_t *)name, n)) {
      log_info("IMedia cmd 0x%X: 播放文件 \"%s\"", cmd, name);
      uint8_t *fdata = NULL;
      size_t flen = 0;
      if (zm_fs_read_file(name, &fdata, &flen) == 0 && fdata && flen) {
        int rc = play_from_mem(fdata, (uint32_t)flen);
        free(fdata);
        return rc == 0 ? 0 : -1;
      }
      log_warn("IMedia cmd 0x%X: 读不到文件 \"%s\"", cmd, name);
      return -1;
    }
    /* 不是文件名（裸字节）→ 落到下方裸音频分支 */
  }

  /* ---- 裸音频流模式（其它测例直接传 MP3/WAV/MIDI 字节；
   *      playMidSound 0x01 / playRealSound 0x02/0x44 也走这里）----
   * r2 = 缓冲  r3 = 长度 */
  if (r2 && r3) {
    uint8_t *raw = malloc(r3);
    if (raw) {
      if (uc_mem_read(uc, r2, raw, r3) == UC_ERR_OK) {
        int is_mp3 = (r3 >= 3 && raw[0] == 'I' && raw[1] == 'D' &&
                      raw[2] == '3');
        log_info("IMedia cmd 0x%X: 裸音频流 len=%u mp3=%d", cmd, r3, is_mp3);
        play_from_mem(raw, r3);
        free(raw);
        return 0;
      }
      free(raw);
    }
    log_error("IMedia cmd 0x%X: 读取音频缓冲失败", cmd);
  }
  return -1; /* 原生 default 返回 -1 */
}

/* IMedia.stop（+0x14）：停止播放 */
uint32_t zm_media_stop(uc_engine *uc) {
  (void)uc;
  log_info("IMedia.stop called");
  release_music();
  return 0;
}

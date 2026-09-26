#include "zm_audio.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../emu.h" /* g_uc（完成回调跳板） */
#include "../../log/log.h"
#include "../../tool/uc_helper.h"
#include "../../traps/emu_root_traps.h"
#include "../fs/zm_file_mgr.h"
#include <stdbool.h>
#include <unicorn/arm.h> /* UC_ARM_REG_*（完成回调跳板） */

/* ---------- SDL_mixer 音频后端（ZMAEE IMedia）----------
 * IMedia.play（+0x10）传入的音频数据一般是 MP3（带 ID3 头），由
 * SDL_mixer 的 Mix_LoadMUS_RW 自动识别格式并解码。
 */

/* 当前正在播放的 music 及其底层数据缓冲。
 * SDL_RWFromMem 不拥有 data，故 data 需在 music 生命周期内保持有效，
 * 下次 play / shutdown 时统一释放。 */
static Mix_Music *g_music = NULL;
static uint8_t *g_music_data = NULL;

/* 声音总开关（默认出声；ZM_SOUND=0 强制静音） */
static int g_sound_on = 0;

/* ZM_SOUND_FORCE=1：**忽略 applet 的声音开关**，始终出声。
 * 兜底/排查用 —— 免得为了听个响还要跟 applet 的开关方向较劲。 */
static int g_sound_force = 0;

/* app 的声音开关（真机里由 Java 侧按 ISetting[+0x18] 的值改媒体音量）。
 * 按固件字面语义：`putBundleInt(bundle, "on", r1)` → 非 0 = 开、0 = 关。
 * 默认 1（没碰过这个接口的 applet 照常出声）。 */
static int g_media_on = 1;

/* 上一次 playMusic 的 loop 标志：BGM 需要"重新起播"时要用 */
static int g_music_loop = 1;

/* 该 applet 用过 ISetting[+0x18]（= 它有自己的声音开关）*/
static int g_app_switch_seen = 0;
/* 用户是否已经点过（触摸过一次）。 */
static int g_user_input_seen = 0;

/* ---- "进去默认关" 规则 ----
 * 00000506 一启动就发 ISetting[+0x18](1)（声音开）并起 BGM，可它的设置页显示
 * 的却是"关"，用户期望的是：**进去静音，点一下开关才出声**。所以对"用过
 * ISetting[+0x18] 的 applet"，在用户第一次触摸之前一律不算数（强制静音）；
 * 用户点过之后，才按 app 自己发的开/关（ISetting / pauseMusic / stop /
 * playMusic）走。这样进去是关、切换开关才打开，两边都能生效。
 * 没用过这个开关的 applet 不受影响（照常出声）。 */
static int sound_allowed(void) {
	if (!g_sound_on)
		return 0; /* ZM_SOUND=0：总闸静音 */
	if (g_sound_force)
		return 1; /* ZM_SOUND_FORCE=1：无视 app 开关 */
	if (g_app_switch_seen && !g_user_input_seen)
		return 0; /* 进去默认关 */
	return g_media_on;
}

/* 按 ZM_SOUND 总闸 + app 开关算出实际音量（音量层） */
static void apply_volume(void) {
	int on = sound_allowed() ? MIX_MAX_VOLUME : 0;
	Mix_VolumeMusic(on);
	Mix_Volume(-1, on);
}

/* 让已加载的 BGM 真正处于"在播"状态：被 halt 过就重新起播，只是暂停就 resume。
 * （旧实现只调 Mix_ResumeMusic，BGM 一旦被 halt 过就再也救不回来 —— 这正是
 *  "怎么弄都没声"的根源。） */
static void ensure_bgm_playing(const char *why) {
	if (!g_music) {
		log_info("BGM(%s): 还没有曲目，等 playMusic", why);
		return;
	}
	if (!Mix_PlayingMusic()) {
		Mix_PlayMusic(g_music, g_music_loop ? -1 : 0);
		log_info("BGM(%s): 重新起播（loop=%d）", why, g_music_loop);
	} else if (Mix_PausedMusic()) {
		Mix_ResumeMusic();
		log_info("BGM(%s): 由暂停恢复（resume）", why);
	} else {
		log_info("BGM(%s): 本来就在播", why);
	}
}

/* 当前"应当静音"（含"进去默认关"未解除的情况）→ 把 BGM 暂停 */
static void pause_bgm_if_muted(const char *why) {
	if (sound_allowed())
		return;
	if (g_music && Mix_PlayingMusic() && !Mix_PausedMusic()) {
		Mix_PauseMusic();
		log_info("BGM(%s): 应静音（%s）→ 暂停",
				 why,
				 (g_app_switch_seen && !g_user_input_seen) ? "进去默认关，尚未点开关"
														   : "app 开关=关");
	}
}

/* 用户首次触摸 → 解除"进去默认关"，并按 app 当前开关状态重新评估一次 */
void zm_audio_note_user_input(void) {
	if (g_user_input_seen)
		return;
	g_user_input_seen = 1;
	log_info("音频: 收到首次用户点击（用过 ISetting 开关=%d, app 开关=%d）→ "
			 "此后按 app 的开关状态发声",
			 g_app_switch_seen,
			 g_media_on);
	apply_volume();
	if (sound_allowed())
		ensure_bgm_playing("首次点击");
	else
		pause_bgm_if_muted("首次点击");
}

/* ---------- 完成回调跳板（RE：loc_32E80 case 16/64 = playMusic）----------
 * 真机在**起新音乐之前**会做：
 *   if (isMusicPlaying()) {
 *     cb = media[+0x10];                                  // 上次 a6 存下的回调
 *     if (cb) cb(media[+0x14]，即上次 a7 那个上下文, 0x7FFF, 0);
 *   }
 *   media[+0x10] = a6;  media[+0x14] = a7;                // 再存新的
 * 即"上一首被打断"的异步通知（0x7FFF 是它的到期间隔/状态码，原样透传）。
 * 音效分支（case 1 / 2）只存 media+8 / media+0xC，**不**回调旧句柄。
 *
 * 我们处在 trap（hook）里，不能嵌套跑客户机代码，所以这里只**排队**；
 * 由主循环（zm_display_event_loop）像定时器那样挂跳板执行：写
 * LR=TR_enter_event_loop + 实参 + PC=cb，Unicorn 跑完自然回到事件循环。 */
typedef struct {
	uint32_t cb, ctx, a1, a2;
} media_pending_cb_t;
static media_pending_cb_t s_pending_cb[4];
static int s_pending_cb_n = 0;

static void queue_completion_cb(uint32_t cb, uint32_t ctx, uint32_t a1, uint32_t a2) {
	if (!cb)
		return;
	if (s_pending_cb_n >= (int)(sizeof(s_pending_cb) / sizeof(s_pending_cb[0]))) {
		log_warn("IMedia: 完成回调队列满，丢弃 cb=0x%X", cb);
		return;
	}
	media_pending_cb_t *e = &s_pending_cb[s_pending_cb_n++];
	e->cb = cb;
	e->ctx = ctx;
	e->a1 = a1;
	e->a2 = a2;
	log_info("IMedia: 排队完成回调 cb=0x%X(ctx=0x%X, 0x%X, 0x%X)", cb, ctx, a1, a2);
}

/* 主循环调用：有排队的完成回调就挂跳板（返回 true 表示"让模拟器去执行"） */
bool zm_media_pending_cb_poll(uc_engine *uc) {
	if (s_pending_cb_n <= 0)
		return false;
	media_pending_cb_t e = s_pending_cb[0];
	for (int i = 1; i < s_pending_cb_n; i++)
		s_pending_cb[i - 1] = s_pending_cb[i];
	s_pending_cb_n--;

	/* 真机调用约定：旧回调 = cb(**ctx**, 0x7FFF, 0)
	 * （R0=旧 ctx、R1=0x7FFF、R2=0）。00000506 的回调 sub_25DC 第一件事就是
	 * `[R0]` 取虚表，所以 R0 必须是那个对象（ctx）。曾经错写成 R0=0x7FFF、
	 * R1=0，直接把 0x7FFF 当对象解引用 → UC_ERR_READ_UNMAPPED 崩溃
	 * （log.txt 里 R15=0x25E4 / R0=0x7FFF 那次）。 */
	uint32_t lr = TR_enter_event_loop;
	uc_reg_write(uc, UC_ARM_REG_LR, &lr);
	uc_reg_write(uc, UC_ARM_REG_R0, &e.ctx);
	uc_reg_write(uc, UC_ARM_REG_R1, &e.a1);
	uc_reg_write(uc, UC_ARM_REG_R2, &e.a2);
	uc_reg_write(uc, UC_ARM_REG_PC, &e.cb);
	log_info("IMedia 完成回调 -> cb=0x%08X(ctx=0x%08X, 0x%08X, 0x%08X)", e.cb, e.ctx, e.a1, e.a2);
	return true;
}

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

	/* ---- 声音总开关（我们的对应物）----
	 * 真机出声与否是**两层**：
	 *   1) 系统/Java 侧音量（AEEJNIBridge 那一头，固件管不着）；
	 *   2) applet 自己的开关 —— 固件 ISetting[+0x18]（键 "on"，见 zm_shell.c）
	 *      配上 IMedia 的 pauseMusic/resumeMusic（+0x18/+0x1C，下面的实现）。
	 * 我们**默认出声**，让第 2 层成为唯一的"开关"：applet 里把声音关掉就真的
	 * 没声、开回来就真的响。早先默认静音 + pause/resume 空实现，会变成"开关
	 * 怎么拨都没反应"（用户实测反馈）。
	 * 要整体静音（不看 applet 设置）设 ZM_SOUND=0。 */
	{
		const char *s = getenv("ZM_SOUND");
		int force_off = 0;
		if (s && *s) {
			char c = s[0];
			force_off = (c == '0' || c == 'n' || c == 'N' || c == 'o' || c == 'O' || c == 'f' ||
						 c == 'F'); /* 0/no/off/false */
		}
		g_sound_on = !force_off;
		{
			const char *f = getenv("ZM_SOUND_FORCE");
			g_sound_force = (f && *f && f[0] != '0') ? 1 : 0;
			if (g_sound_force) {
				g_media_on = 1;
				log_info("zm_audio_init: ZM_SOUND_FORCE=1 → 忽略 applet 的声音开关，"
						 "始终出声");
			}
		}
		apply_volume();
		if (g_sound_on)
			log_info("zm_audio_init: SDL_mixer 就绪，音频输出**开启**（默认；"
					 "出声与否由 applet 自己的声音开关决定：pauseMusic/resumeMusic）");
		else
			log_info("zm_audio_init: SDL_mixer 就绪，音频输出被 ZM_SOUND 强制**静音**");
	}
	return 0;
}

void zm_audio_shutdown(void) {
	release_music();
	Mix_CloseAudio();
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

/* IMedia 通用 stub（未实现槽） */
uint32_t
zm_media_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
	(void)uc;
	log_info("media stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1, r2, r3);
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
static int play_from_mem(const uint8_t *data, uint32_t len, int loop) {
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
	/* loop 来自真机分发器的 sp[0]（`loop = (sp[0] != 0)`，见 zm_media_command）：
	 * 非 0 → 无限循环（SDL_mixer 的 -1）。 */
	g_music_loop = loop ? 1 : 0;
	if (Mix_PlayMusic(g_music, g_music_loop ? -1 : 0) == -1) {
		log_error("Mix_PlayMusic failed: %s", Mix_GetError());
		release_music();
		return -1;
	}
	/* playMusic = app 明确要出声 → 记为"开"；音量跟随开关，
	 * 若当前应当静音（app 开关=关 / 进去还没点过开关）则起播即暂停。 */
	g_media_on = 1;
	apply_volume();
	pause_bgm_if_muted("playMusic");
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

uint32_t
zm_media_command(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
	(void)r0;
	/* 掌萌 zmapp AEE 媒体层：+0x10（loc_32E80）实为命令分发器，
	 * 原型 int loc_32E80(void *self, int cmd, void *arg1, void *arg2, ...)；
	 * cmd 取 r1 低 16 位。+0x54（sub_33128）是同一分发器的 thunk，亦走此处。
	 *
	 * 注意第 5 个参数起在**栈上**：00000506 的调用链
	 *   sub_904C(this, 曲目索引, R2) → sub_26CC(media, 名字, R2, this)
	 *   → sub_2630(media, 名字, 长度, cmd=0x10, R2, this)
	 * 把 R2 与 this 分别压到 sp[0]/sp[1]，最后才 BLX 到本分发器。
	 * 以前我们完全不读栈，等于把它们丢掉 —— 目前照实记录，语义待固件反编译确认。
	 */
	uint16_t cmd = (uint16_t)(r1 & 0xFFFF);
	/* 栈上实参的语义（RE：固件分发器 loc_32E80，`: .data:0x640F4` 即 IMedia 槽
	 * +0x10）： sp[0] = **loop 标志**：各 play 分支里做 `(sp[0] != 0)`
	 * 布尔化后，作为 Java 侧 playMusic / playMidSound / playRealSound 的最后一个
	 * int （Android 侧据此决定是否循环；汇编是 `SUBS R3,#1 / SBCS`）； sp[1] =
	 * **完成回调**（music 存 media+0x10，音效存 media+8）； sp[2] =
	 * **回调上下文**（music 存 media+0x14）。 另外 case
	 * 16/64（playMusic）在起播前：若 isMusicPlaying 且旧回调非 0， 会先调
	 * `旧回调(旧ctx, 0x7FFF, 0)` 通知"上一首被打断"。 */
	uint32_t sp0_loop = sp ? uc_read32(uc, sp) : 0;
	uint32_t sp1_cb = sp ? uc_read32(uc, sp + 4) : 0;
	uint32_t sp2_ctx = sp ? uc_read32(uc, sp + 8) : 0;
	int loop = (sp0_loop != 0);
	log_info("IMedia cmd 分发器: cmd=0x%X  r2=0x%X  r3=0x%X  loop=%d cb=0x%X ctx=0x%X",
			 cmd,
			 r2,
			 r3,
			 loop,
			 sp1_cb,
			 sp2_ctx);

	/* 真机同位置做的两件事（RE：loc_32E80 case 16/64 与 case 1/2/68）：
	 *   1) playMusic：若正在播放且 media+0x10 存有旧回调 → 先通知它"被打断"；
	 *   2) 把新回调/上下文存进 media 对象字段（music: +0x10/+0x14，
	 *      音效: +0x8/+0xC）。 */
	if (r0) {
		if (cmd == 0x10 || cmd == 0x40) { /* playMusic */
			uint32_t old_cb = uc_read32(uc, r0 + 0x10);
			uint32_t old_ctx = uc_read32(uc, r0 + 0x14);
			if (old_cb && g_music && Mix_PlayingMusic())
				queue_completion_cb(old_cb, old_ctx, 0x7FFF, 0);
			uc_write32(uc, r0 + 0x10, sp1_cb);
			uc_write32(uc, r0 + 0x14, sp2_ctx);
		} else if (cmd == 0x01 || cmd == 0x02 || cmd == 0x44) {
			uc_write32(uc, r0 + 0x8, sp1_cb);
			uc_write32(uc, r0 + 0xC, sp2_ctx);
		}
	}

	/* loadSound / unloadSound：预加载/卸载，emu 按需播放，noop */
	if (cmd == 0x42 || cmd == 0x43) {
		log_info("IMedia cmd 0x%X %s (noop)", cmd, cmd == 0x42 ? "loadSound" : "unloadSound");
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
			log_info("IMedia cmd 0x%X: 播放文件 \"%s\"%s", cmd, name, loop ? "（循环）" : "");
			uint8_t *fdata = NULL;
			size_t flen = 0;
			if (zm_fs_read_file(name, &fdata, &flen) == 0 && fdata && flen) {
				int rc = play_from_mem(fdata, (uint32_t)flen, loop);
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
				int is_mp3 = (r3 >= 3 && raw[0] == 'I' && raw[1] == 'D' && raw[2] == '3');
				log_info("IMedia cmd 0x%X: 裸音频流 len=%u mp3=%d loop=%d", cmd, r3, is_mp3, loop);
				play_from_mem(raw, r3, loop);
				free(raw);
				return 0;
			}
			free(raw);
		}
		log_error("IMedia cmd 0x%X: 读取音频缓冲失败", cmd);
	}
	return -1; /* 原生 default 返回 -1 */
}

/* IMedia.stop（+0x14）：stopAllRealSound */
uint32_t zm_media_stop(uc_engine *uc) {
	uint32_t lr = 0;
	uc_reg_read(uc, UC_ARM_REG_LR, &lr);
	/* +0x14 = AEEJNIBridge.stopAllRealSound()V（RE 见 emu_media_traps.h）。
	 *
	 * 00000506 把它当"关声音"用：它的关声音包装是
	 *   sub_96AC(state) → state[0x60] → sub_25F4：obj = [w+4];
	 *                    若 obj 与 [w+0xC] 都非 0 → obj->vt[+0x14]()   ←
	 * 就是本函数 所以这里除了停掉全部音效通道，**也要把 BGM
	 * 停掉**，否则就是用户实测的 "开关拨过去、音效没了，BGM 还在循环"。停用
	 * pause（不 halt、不释放）， 曲目保留 —— 之后 playMusic/resumeMusic/ISetting
	 * 都能把它再拉起来。 */
	Mix_HaltChannel(-1);
	if (g_music && Mix_PlayingMusic() && !Mix_PausedMusic()) {
		Mix_PauseMusic();
		log_info("IMedia.stop/stopAllRealSound() lr=0x%X: 音效通道已停 + BGM 已暂停"
				 "（曲目保留，可再起）",
				 lr);
	} else {
		log_info("IMedia.stop/stopAllRealSound() lr=0x%X: 音效通道已停"
				 "（BGM 本来就没在播）",
				 lr);
	}
	return 0;
}

/* IMedia[+0x18] = AEEJNIBridge.pauseMusic()
 *
 * RE：固件 sub_32CE8 是薄壳 `call JNI(com/zmapp/aee/AEEJNIBridge, "pauseMusic",
 * "()V")`（字面量解出来的字符串即此）。同族 +0x1C = resumeMusic、
 * +0x14 = stopAllRealSound（见 emu_media_traps.h 的表）。
 *
 * 这是 applet"声音开关=关"真正落地的那一枪：00000506 的关声音函数是
 *   ISetting[+0x18](0)        ; 偏好写"关"
 *   IMedia[+0x18]()           ; ← 让媒体层闭嘴（本函数）
 * 以前这里是空 stub（返回 0），所以 applet 明明显示"声音：关"、BGM 照放 ——
 * 正是用户反馈的现象。 */
uint32_t zm_media_pause_music(uc_engine *uc) {
	uint32_t lr = 0; /* applet 调用点：用来判断"是哪个界面/哪支函数在关声音" */
	uc_reg_read(uc, UC_ARM_REG_LR, &lr);
	/* 只记日志 → 交给收敛点按 app 开关决定（该 app 会把它和方向相反的 ISetting
	 * 一起发出来，照字面执行会变成"开关怎么拨都没声"）。 */
	if (g_sound_force) {
		log_info("IMedia.pauseMusic() lr=0x%X: ZM_SOUND_FORCE 生效 → 忽略本次暂停", lr);
		return 0;
	}
	if (g_music && Mix_PlayingMusic() && !Mix_PausedMusic())
		Mix_PauseMusic();
	log_info("IMedia.pauseMusic() lr=0x%X → BGM 暂停", lr);
	return 0;
}

/* IMedia[+0x1C] = AEEJNIBridge.resumeMusic()：与 pauseMusic 成对，
 * applet 把声音开回来时调用。 */
uint32_t zm_media_resume_music(uc_engine *uc) {
	uint32_t lr = 0;
	uc_reg_read(uc, UC_ARM_REG_LR, &lr);
	log_info("IMedia.resumeMusic() lr=0x%X → 恢复 BGM", lr);
	g_media_on = 1; /* resumeMusic = app 明确要出声 → 记为"开" */
	apply_volume();
	ensure_bgm_playing("resumeMusic");
	return 0;
}

/* ISetting[+0x18]（键 "on"）落到音频侧。
 *
 * 真机语义（RE 固件 sub_33FB4）：把 "on" 放进 bundle、postMessageToJava(5)，
 * 由 **Java 侧**去调该应用的媒体音量。固件自己不出声、也不静音，所以这边必须
 * 替 Java 侧把这件事做掉。
 *
 * ⚠ 值的解释：**非 0 = 关声音，0 = 开声音**（不是字面意义上的 "on=1 就开"）。
 * 实测依据（00000506，用户 GUI 复现）：
 *   - 启动时它的设置页显示"关"，它调 [+0x18](1)；
 *   - 把设置拨到"开"后，它调 [+0x18](0)（并调 pauseMusic）。
 * 若按字面理解就会正好反相：设置关却有声音、拨到开反而没声。
 * 所以这里同向解释（= 扮演 Java 侧按该 app 的实际约定改媒体音量）：
 *   非 0 → 音量 0 + 暂停 BGM（"彻底没声"，可由下一句 resume）；
 *   0    → 恢复音量（受 ZM_SOUND 总闸约束）+ resume。 */
void zm_audio_set_sound_flag(uint32_t raw) {
	/* 固件字面语义：putBundleInt(bundle, "on", r1) → 非 0 = 开 */
	int on = (raw != 0);
	g_app_switch_seen = 1; /* 它有自己的声音开关 → 启用"进去默认关"规则 */
	if (g_sound_force) {
		log_info("ISetting[0x18]=%u（原意=%s）被 ZM_SOUND_FORCE=1 覆盖 → 保持出声",
				 raw,
				 on ? "开" : "关");
		g_media_on = 1;
	} else {
		g_media_on = on;
	}
	apply_volume();
	if (sound_allowed())
		ensure_bgm_playing("ISetting[0x18]");
	else
		pause_bgm_if_muted("ISetting[0x18]");
	log_info("ISetting[0x18]=%u → 声音%s（实际音量=%d, ZM_SOUND=%d, 已点过=%d）",
			 raw,
			 on ? "开" : "关",
			 Mix_VolumeMusic(-1),
			 g_sound_on,
			 g_user_input_seen);
}

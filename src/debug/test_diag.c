#include "../emu.h"
#include "../event.h"
#include "../log/log.h"
#include "../zmaee/audio/zm_audio.h"
#include "../zmaee/fs/zm_file.h"
#include "../zmaee/fs/zm_file_mgr.h"
#include "../zmaee/gfx/zm_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void zm_diag_dump_buttons(uc_engine *uc) {
	const char *db = getenv("ZM_DUMP_BUTTONS");
	if (db && *db && g_instance) {
		for (uint32_t i = 0; i < 25; i++) {
			uint32_t off = g_instance + 124 + i * 16;
			uint32_t r[4];
			if (uc_mem_read(uc, off, r, 16) == UC_ERR_OK)
				log_info("按钮[%2u] rect=(%u,%u,%u,%u) center=(%u,%u)",
						 i,
						 r[0],
						 r[1],
						 r[2],
						 r[3],
						 r[0] + r[2] / 2,
						 r[1] + r[3] / 2);
		}
	}
}

// void zm_diag_audio_test(uc_engine *g_uc) {
//   const char *atest = getenv("ZM_AUDIO_TEST");
//   if (atest && *atest) {
//     uint32_t count = zm_fs_get_resource_count();
//     uint32_t idx = 0;
//     if (strcmp(atest, "random") == 0) {
//       srand((unsigned)time(NULL));
//       idx = count ? (uint32_t)(rand() % count) : 0;
//     } else {
//       idx = (uint32_t)strtoul(atest, NULL, 0);
//     }
//     uint32_t rsize = 0;
//     const uint8_t *rdata = zm_fs_get_resource(idx, &rsize);
//     if (rdata && rsize && rsize <= ZMR_SIZE) {
//       uc_mem_write(g_uc, ZMR_BASE, rdata, rsize);
//       log_info("音频自测：播放资源 %u/%u  size=%u", idx, count, rsize);
//       zm_media_command(g_uc, 0, 0x10, ZMR_BASE, rsize);
//     } else {
//       log_warn("音频自测：资源 %u 不可用 (count=%u)", idx, count);
//     }
//   }
// }

void zm_diag_run_event_loop(void) {
	uint32_t hold_ms = 0;
	const char *env = getenv("ZM_GFX_HOLD_MS");
	if (env && *env)
		hold_ms = (uint32_t)strtoul(env, NULL, 0);
	zm_display_event_loop(on_touch_click, hold_ms);
}
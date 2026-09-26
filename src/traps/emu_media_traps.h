#ifndef EMU_MEDIA_TRAPS_H
#define EMU_MEDIA_TRAPS_H

#include "../emu_mem_layout.h"

/* -------------------- MEDIA_VT_ADDR 枚举 -------------------- */
/* ZMAEE IMedia 原生虚表槽位（基址 MEDIA_VT_ADDR，25 槽，止于 +0x60） */
enum ZM_MEDIA_VT : uint32_t {
	ZM_Media_AddRef = 0x00U,
	ZM_Media_Release = 0x04U,
	ZM_Media_x08 = 0x08U,
	ZM_Media_x0C = 0x0CU,
	ZM_Media_play = 0x10U,
	ZM_Media_stop = 0x14U,
	ZM_Media_x18 = 0x18U,
	ZM_Media_x1C = 0x1CU,
	ZM_Media_x20 = 0x20U,
	ZM_Media_x24 = 0x24U,
	ZM_Media_x28 = 0x28U,
	ZM_Media_x2C = 0x2CU,
	ZM_Media_x30 = 0x30U,
	ZM_Media_x34 = 0x34U,
	ZM_Media_x38 = 0x38U,
	ZM_Media_x3C = 0x3CU,
	ZM_Media_x40 = 0x40U,
	ZM_Media_x44 = 0x44U,
	ZM_Media_x48 = 0x48U,
	ZM_Media_x4C = 0x4CU,
	ZM_Media_x50 = 0x50U,
	ZM_Media_x54 = 0x54U,
	ZM_Media_x58 = 0x58U
};

/* ---- ZMAEE IMedia 原生虚表（RE：g_aee_media_vtbl @ .data:0x640E4，25 槽）
 * ---- IMedia 即音频（用户确认）。偏移逐槽按 RE（JNI 名解自 libaee.so 里各薄壳
 *      的 PC 相对字面量：class = com/zmapp/aee/AEEJNIBridge）：
 *   +0x00 sub_32BC8(AddRef)  +0x04 sub_32BCC(Release)
 *   +0x08 sub_32BD0（固定返回 -1）      +0x0C sub_32BD8（固定返回 6）
 *   +0x10 sub_32E80 = 命令分发器 loc_32E80（cmd 取 r1 低 16 位）：
 *         0x10/0x40 playMusic(String,int)  0x41 playSound(String)
 *         0x01 playMidSound(裸MIDI)       0x02/0x44 playRealSound(裸音频)
 *         0x42 loadSound / 0x43 unloadSound；其余 default 返回 -1
 *   +0x14 sub_32D0C = AEEJNIBridge.stopAllRealSound()V
 *   +0x18 sub_32CE8 = AEEJNIBridge.pauseMusic()V   ← applet"声音开关=关"落地处
 *   +0x1C sub_32CC4 = AEEJNIBridge.resumeMusic()V  ← "声音开关=开"
 *   +0x20 sub_32DF8（内部：加载/缓冲相关）  +0x24 sub_32DD8 = getMusicProgress()I
 *   +0x28 sub_32DB4 = setMusicProgress(I)I  +0x2C sub_32D4C（另一组分发：cmd 0x45..0x4C、2）
 *   +0x30 sub_32C60  +0x34 sub_32C00（内部：释放 [this+0x1C] 的对象）
 *   +0x38 sub_32BDC（固定 -1）  +0x3C sub_32BE4(0)  +0x40 sub_32BE8（固定 -1）
 *   +0x44 sub_32BF0(0)  +0x48 sub_32BF4(0)  +0x4C sub_32BF8(0)  +0x50 sub_32BFC(0)
 *   +0x54 sub_33128 = 同一分发器 loc_32E80 的 thunk（亦走 zm_media_command）
 *   +0x58 sub_32D44 = stopAllRealSound 的 thunk（尾调 +0x14）
 * 已实现：+0x10/+0x54 → zm_media_command；+0x14 → zm_media_stop；
 *         +0x18 → zm_media_pause_music；+0x1C → zm_media_resume_music；
 *         常量槽（+0x08 -1、+0x38/-1、+0x40/-1）在 trap.c 里直接返常量；
 *         其余接 zm_media_stub（返回 0，与 RE 的常量槽一致）。 */
enum ZM_MEDIA_TRAPS : uint32_t {
	TR_media_AddRef = TRAP(MEDIA_VT_ADDR + ZM_Media_AddRef),
	TR_media_Release = TRAP(MEDIA_VT_ADDR + ZM_Media_Release),
	TR_media_x08 = TRAP(MEDIA_VT_ADDR + ZM_Media_x08),
	TR_media_x0C = TRAP(MEDIA_VT_ADDR + ZM_Media_x0C),
	TR_media_play = TRAP(MEDIA_VT_ADDR + ZM_Media_play),
	TR_media_stop = TRAP(MEDIA_VT_ADDR + ZM_Media_stop),
	TR_media_x18 = TRAP(MEDIA_VT_ADDR + ZM_Media_x18),
	TR_media_x1C = TRAP(MEDIA_VT_ADDR + ZM_Media_x1C),
	TR_media_x20 = TRAP(MEDIA_VT_ADDR + ZM_Media_x20),
	TR_media_x24 = TRAP(MEDIA_VT_ADDR + ZM_Media_x24),
	TR_media_x28 = TRAP(MEDIA_VT_ADDR + ZM_Media_x28),
	TR_media_x2C = TRAP(MEDIA_VT_ADDR + ZM_Media_x2C),
	TR_media_x30 = TRAP(MEDIA_VT_ADDR + ZM_Media_x30),
	TR_media_x34 = TRAP(MEDIA_VT_ADDR + ZM_Media_x34),
	TR_media_x38 = TRAP(MEDIA_VT_ADDR + ZM_Media_x38),
	TR_media_x3C = TRAP(MEDIA_VT_ADDR + ZM_Media_x3C),
	TR_media_x40 = TRAP(MEDIA_VT_ADDR + ZM_Media_x40),
	TR_media_x44 = TRAP(MEDIA_VT_ADDR + ZM_Media_x44),
	TR_media_x48 = TRAP(MEDIA_VT_ADDR + ZM_Media_x48),
	TR_media_x4C = TRAP(MEDIA_VT_ADDR + ZM_Media_x4C),
	TR_media_x50 = TRAP(MEDIA_VT_ADDR + ZM_Media_x50),
	TR_media_x54 = TRAP(MEDIA_VT_ADDR + ZM_Media_x54),
	TR_media_x58 = TRAP(MEDIA_VT_ADDR + ZM_Media_x58)
};

#endif /* EMU_MEDIA_TRAPS_H */

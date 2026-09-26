#include "zm_stat.h"

#include "../log/log.h"
#include <stdlib.h>

/* 定长开放寻址哈希表：key = 槽位偏移 / 点击坐标打包，value = 次数。
 * 默认关闭（未开 ZM_STAT 时只有一次分支判断，零开销）。 */
#define ZM_STAT_CAP 2048

typedef struct {
	uint32_t key;
	uint32_t cnt;
} zm_stat_ent_t;

/* ① 全程槽位统计  ② 点击坐标统计  ③ **每次点击期间触发的外部调用**（本文件核心） */
static zm_stat_ent_t s_trap[ZM_STAT_CAP];
static zm_stat_ent_t s_touch[ZM_STAT_CAP];
static zm_stat_ent_t s_click[ZM_STAT_CAP];

static uint32_t s_trap_used = 0, s_trap_total = 0;
static uint32_t s_touch_used = 0, s_touch_total = 0;
static uint32_t s_click_used = 0, s_click_total = 0;

/* 槽位偏移 → 归属对象名（实现见文件末尾） */
const char *zm_stat_slot_name(uint32_t key, uint32_t *slot);

static int s_on = 0;
static int s_click_seq = 0;	 /* 第几次点击 */
static int s_click_open = 0; /* 是否有正在累积的"本次点击"桶 */
static uint32_t s_last_x = 0, s_last_y = 0;

static uint32_t zm_stat_hash(uint32_t k) {
	k ^= k >> 16;
	k *= 0x7feb352dU;
	k ^= k >> 15;
	k *= 0x846ca68bU;
	k ^= k >> 16;
	return k;
}

static void zm_stat_bump(zm_stat_ent_t *tab, uint32_t *used, uint32_t *total, uint32_t key) {
	uint32_t i = zm_stat_hash(key) % ZM_STAT_CAP;
	for (uint32_t n = 0; n < ZM_STAT_CAP; n++) {
		uint32_t j = (i + n) % ZM_STAT_CAP;
		if (tab[j].cnt == 0) {
			tab[j].key = key;
			tab[j].cnt = 1;
			(*used)++;
			(*total)++;
			return;
		}
		if (tab[j].key == key) {
			tab[j].cnt++;
			(*total)++;
			return;
		}
	}
}

static void zm_stat_clear(zm_stat_ent_t *tab, uint32_t *used, uint32_t *total) {
	for (uint32_t i = 0; i < ZM_STAT_CAP; i++)
		tab[i].key = tab[i].cnt = 0;
	*used = 0;
	*total = 0;
}

void zm_stat_init(void) {
	const char *s = getenv("ZM_STAT");
	s_on = (s && *s && s[0] != '0') ? 1 : 0;
	if (s_on)
		log_info("调用统计已开启（ZM_STAT=1）：全程槽位榜 + 点击坐标榜 + "
				 "**每次点击触发的外部调用清单**");
}

void zm_stat_trap(uint32_t slot) {
	if (!s_on)
		return;
	zm_stat_bump(s_trap, &s_trap_used, &s_trap_total, slot);
	/* 同时累计进"本次点击"桶：回答"我点这一下到底触发了什么" */
	if (s_click_open)
		zm_stat_bump(s_click, &s_click_used, &s_click_total, slot);
}

void zm_stat_touch(uint32_t x, uint32_t y) {
	if (!s_on)
		return;
	zm_stat_bump(s_touch, &s_touch_used, &s_touch_total, ((y & 0xFFFFu) << 16) | (x & 0xFFFFu));

	/* 打印上一次点击期间的外部调用清单，然后开一个新桶 */
	if (s_click_open) {
		log_info("===== 第 %d 次点击 (%u,%u) 期间触发的外部调用：共 %u 次 =====",
				 s_click_seq,
				 s_last_x,
				 s_last_y,
				 s_click_total);
		for (uint32_t i = 0; i < ZM_STAT_CAP; i++) {
			if (s_click[i].cnt) {
				uint32_t slot = 0;
				const char *obj = zm_stat_slot_name(s_click[i].key, &slot);
				if (obj)
					log_info("       %s +0x%X  ×%u", obj, slot, s_click[i].cnt);
				else
					log_info("       槽位 0x%X  ×%u", s_click[i].key, s_click[i].cnt);
			}
		}
		zm_stat_clear(s_click, &s_click_used, &s_click_total);
	}
	s_click_seq++;
	s_click_open = 1;
	s_last_x = x;
	s_last_y = y;
	log_info("===== 第 %d 次点击 (%u,%u)：开始记录此后触发的外部调用 =====", s_click_seq, x, y);
}

/* 槽位 → 归属对象名。
 * 关键点：SHIM_VT_BASE == SHIM_BASE，而 TRAP(idx) = TRAMP_BASE + (idx - SHIM_BASE)，
 * 所以 `trap_addr - TRAMP_BASE` 本身就是"相对虚表基址"的槽位偏移
 * （如 0x1910 → IMedia +0x10）。各基址取自 emu_mem_layout.h。 */
static const struct {
	const char *name;
	uint32_t off;  /* 该虚表相对 SHIM_VT_BASE 的基址 */
	uint32_t size; /* 槽表跨度（用于归组） */
} s_vt_map[] = {
	{"根表 ROOT", 0x000U, 0x200U},
	{"IShell", 0x400U, 0x100U},
	{"IFileMgr", 0x500U, 0x40U},
	{"IFile", 0x580U, 0x30U},
	{"INetMgr", 0x800U, 0x30U},
	{"ITapi", 0x880U, 0x30U},
	{"ICbkObj", 0x1000U, 0x40U},
	{"IDllObj", 0x1100U, 0x40U},
	{"IDisplay", 0x1600U, 0xE8U},
	{"IBitmap", 0x1700U, 0x20U},
	{"ISetting", 0x1800U, 0x40U},
	{"IMedia", 0x1900U, 0x60U},
	{"IUtil", 0x1A00U, 0x40U},
	{"IZip", 0x1B00U, 0x40U},
	{"IImage", 0x2000U, 0x40U},
	{"ISurface", 0x2200U, 0x40U},
};

const char *zm_stat_slot_name(uint32_t key, uint32_t *slot) {
	for (unsigned i = 0; i < sizeof(s_vt_map) / sizeof(s_vt_map[0]); i++) {
		if (key >= s_vt_map[i].off && key < s_vt_map[i].off + s_vt_map[i].size) {
			*slot = key - s_vt_map[i].off;
			return s_vt_map[i].name;
		}
	}
	*slot = key;
	return NULL;
}

static void zm_stat_dump_table(
	const char *title, zm_stat_ent_t *tab, uint32_t used, uint32_t total, int is_touch) {
	if (!s_on)
		return;
	log_info("===== %s：累计 %u 次，%u 个不同目标 =====", title, total, used);
	/* 上限 64：applet 一局通常只用到 40 多个槽，全列出来便于盘"哪些是 stub" */
	for (int k = 0; k < 64; k++) {
		int best = -1;
		uint32_t bestc = 0;
		for (uint32_t i = 0; i < ZM_STAT_CAP; i++) {
			if (tab[i].cnt > bestc) {
				bestc = tab[i].cnt;
				best = (int)i;
			}
		}
		if (best < 0)
			break;
		if (is_touch) {
			uint32_t key = tab[best].key;
			log_info("  #%2d  点击 (%u,%u)  ×%u", k + 1, key & 0xFFFFu, key >> 16, bestc);
		} else {
			uint32_t slot = 0;
			const char *obj = zm_stat_slot_name(tab[best].key, &slot);
			if (obj)
				log_info("  #%2d  %s +0x%X  ×%u", k + 1, obj, slot, bestc);
			else
				log_info("  #%2d  槽位 0x%X  ×%u", k + 1, tab[best].key, bestc);
		}
		tab[best].cnt = 0; /* 取出即清，避免重复 */
	}
}

void zm_stat_dump(void) {
	if (!s_on)
		return;
	/* 最后一次点击的桶也要收尾 */
	if (s_click_open && s_click_total) {
		log_info("===== 第 %d 次点击 (%u,%u) 期间触发的外部调用：共 %u 次 =====",
				 s_click_seq,
				 s_last_x,
				 s_last_y,
				 s_click_total);
		for (uint32_t i = 0; i < ZM_STAT_CAP; i++) {
			if (s_click[i].cnt) {
				uint32_t slot = 0;
				const char *obj = zm_stat_slot_name(s_click[i].key, &slot);
				if (obj)
					log_info("       %s +0x%X  ×%u", obj, slot, s_click[i].cnt);
				else
					log_info("       槽位 0x%X  ×%u", s_click[i].key, s_click[i].cnt);
			}
		}
	}
	zm_stat_dump_table("槽位调用统计", s_trap, s_trap_used, s_trap_total, 0);
	zm_stat_dump_table("点击坐标统计", s_touch, s_touch_used, s_touch_total, 1);
}

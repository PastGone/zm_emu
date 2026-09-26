#include "../../include/u_fmt.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file u_vsnprintf_core.c
 * @brief 格式化核心引擎 —— 整个 printf 家族的一级基本函数
 *
 * 做法：解析格式串 → 用 u_va 逐个从客户机取标量 → 交给宿主做**单次**
 *       转换 → 拼进输出缓冲。既复用宿主 libc 的浮点/本地化能力，
 *       又完全绕开 va_list 的 ABI 差异与地址空间隔离。
 *
 * 本文件同时提供若干 static 辅助（输出缓冲、转换说明构造），
 * 它们只服务于本引擎，故不单独成文件。
 */

/* -------------------- 输出缓冲 -------------------- */

typedef struct {
	char *out;
	size_t cap;
	size_t oi; /* 已产出字符数（可能 >= cap） */
	int total; /* 与 oi 同步的 int 版本，供 %n 使用 */
} u_fmtbuf;

static void fb_write(u_fmtbuf *fb, const char *s, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (fb->out && fb->cap > 1 && fb->oi < fb->cap - 1)
			fb->out[fb->oi] = s[i];
		fb->oi++;
	}
	fb->total += (int)n;
}

static void fb_pad(u_fmtbuf *fb, char c, int n) {
	for (int i = 0; i < n; i++)
		fb_write(fb, &c, 1);
}

static void fb_finish(u_fmtbuf *fb) {
	if (!fb->out || fb->cap == 0)
		return;
	size_t t = (fb->oi < fb->cap) ? fb->oi : fb->cap - 1;
	fb->out[t] = '\0';
}

/* -------------------- 转换说明构造 -------------------- */

enum { LM_NONE = 0, LM_hh, LM_h, LM_l, LM_ll, LM_L, LM_j, LM_z, LM_t };

static const char *lm_str(int lm) {
	switch (lm) {
	case LM_hh:
		return "hh";
	case LM_h:
		return "h";
	case LM_l:
		return "l";
	case LM_ll:
		return "ll";
	case LM_L:
		return "L";
	case LM_j:
		return "j";
	case LM_z:
		return "z";
	case LM_t:
		return "t";
	default:
		return "";
	}
}

static void sputs(char *spec, size_t cap, size_t *n, const char *s) {
	while (*s && *n + 1 < cap)
		spec[(*n)++] = *s++;
	spec[*n] = '\0';
}

static void sputi(char *spec, size_t cap, size_t *n, int v) {
	char t[16];
	snprintf(t, sizeof(t), "%d", v);
	sputs(spec, cap, n, t);
}

static int is_float_conv(char c) {
	return (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G' || c == 'a' ||
			c == 'A');
}

/* -------------------- 核心格式化 -------------------- */

int u_vsnprintf_core(uc_engine *uc, u_va *va, char *out, size_t cap, const char *fmt) {
	u_fmtbuf fb;
	fb.out = out;
	fb.cap = cap;
	fb.oi = 0;
	fb.total = 0;

	if (out && cap > 0)
		out[0] = '\0';
	if (!fmt)
		return 0;

	/* 允许 va == NULL：取参一律返回 0，便于"只算长度"的场景 */
	u_va dummy;
	if (!va) {
		u_va_start_array(&dummy, NULL, 0);
		va = &dummy;
	}

	const char *p = fmt;
	char tmp[1024];

	while (*p) {
		if (*p != '%') {
			const char *start = p;
			while (*p && *p != '%')
				p++;
			fb_write(&fb, start, (size_t)(p - start));
			continue;
		}

		p++; /* 吃掉 '%' */
		if (*p == '%') {
			fb_write(&fb, "%", 1);
			p++;
			continue;
		}
		if (*p == '\0')
			break;

		/* ---- 解析 flags ---- */
		char flags[8];
		size_t fni = 0;
		int left = 0;
		while (*p && strchr("-+ #0", *p) && fni + 1 < sizeof(flags)) {
			if (*p == '-')
				left = 1;
			flags[fni++] = *p++;
		}
		flags[fni] = '\0';

		/* ---- 解析 width ---- */
		int width = -1;
		if (*p == '*') {
			p++;
			int w = (int)u_va_i32(va);
			if (w < 0) {
				left = 1;
				w = -w;
				/* 去掉 '0' 标志：左对齐时 0 无意义。
				 * 必须用 '\0' 而非 ' ' 作哨兵 —— ' ' 本身是合法的空格标志
				 * （"% d" 表示为正数前留一个空格），用 ' ' 当哨兵会把它一并吞掉。 */
				for (size_t i = 0; i < fni; i++)
					if (flags[i] == '0')
						flags[i] = '\0';
			}
			width = w;
		} else if (isdigit((unsigned char)*p)) {
			int w = 0;
			while (isdigit((unsigned char)*p))
				w = w * 10 + (*p++ - '0');
			width = w;
		}

		/* ---- 解析 precision ---- */
		int prec = -1;
		if (*p == '.') {
			p++;
			if (*p == '*') {
				p++;
				int pv = (int)u_va_i32(va);
				if (pv >= 0)
					prec = pv; /* 负精度 == 省略精度 */
			} else {
				int pv = 0;
				while (isdigit((unsigned char)*p))
					pv = pv * 10 + (*p++ - '0');
				prec = pv; /* "." 后无数字即精度 0 */
			}
		}

		/* ---- 解析 length ---- */
		int lm = LM_NONE;
		if (*p == 'h') {
			p++;
			if (*p == 'h') {
				p++;
				lm = LM_hh;
			} else {
				lm = LM_h;
			}
		} else if (*p == 'l') {
			p++;
			if (*p == 'l') {
				p++;
				lm = LM_ll;
			} else {
				lm = LM_l;
			}
		} else if (*p == 'L') {
			p++;
			lm = LM_L;
		} else if (*p == 'j') {
			p++;
			lm = LM_j;
		} else if (*p == 'z') {
			p++;
			lm = LM_z;
		} else if (*p == 't') {
			p++;
			lm = LM_t;
		}

		char conv = *p++;
		if (conv == '\0')
			break;

		/* ---- 组装 spec（转换符稍后单独追加） ---- */
		char spec[48];
		size_t sn = 0;
		spec[0] = '\0';
		sputs(spec, sizeof(spec), &sn, "%");
		/* '\0' 表示"该标志已被剔除"（负 * 宽度去掉 '0' 的情况），跳过即可 */
		for (size_t i = 0; i < fni; i++) {
			if (flags[i] != '\0')
				sputs(spec, sizeof(spec), &sn, (char[2]){flags[i], 0});
		}

		/* %p：客户机是 32 位，必须输出 "0x" + 8 位十六进制，不能走宿主的 64 位 %p。
		 * 注意 glibc 的 '#' 前缀 "0x" 会计入 width，所以总宽要取 2+8=10。 */
		char real_conv = conv;
		if (conv == 'p') {
			lm = LM_NONE;
			real_conv = 'x';
			sputs(spec, sizeof(spec), &sn, "#");
			if (width < 0) {
				width = 10;
				sputs(spec, sizeof(spec), &sn, "0");
			}
		}

		if (width >= 0)
			sputi(spec, sizeof(spec), &sn, width);
		if (prec >= 0) {
			sputs(spec, sizeof(spec), &sn, ".");
			sputi(spec, sizeof(spec), &sn, prec);
		}

		if (is_float_conv(real_conv))
			sputs(spec, sizeof(spec), &sn, (lm == LM_L) ? "L" : "");
		else if (real_conv != 'p')
			sputs(spec, sizeof(spec), &sn, lm_str(lm));

		/* ---- 按转换符取参并输出 ---- */
		switch (real_conv) {
		case 'd':
		case 'i': {
			size_t save = sn;
			sputs(spec, sizeof(spec), &sn, (char[2]){real_conv, 0});
			switch (lm) {
			case LM_ll:
				snprintf(tmp, sizeof(tmp), spec, (long long)u_va_i64(va));
				break;
			case LM_l:
				snprintf(tmp, sizeof(tmp), spec, (long)u_va_i32(va));
				break;
			case LM_j:
				snprintf(tmp, sizeof(tmp), spec, (intmax_t)u_va_i64(va));
				break;
			case LM_z:
			case LM_t:
				snprintf(tmp, sizeof(tmp), spec, (ptrdiff_t)u_va_i32(va));
				break;
			case LM_hh:
				snprintf(tmp, sizeof(tmp), spec, (int)(signed char)u_va_i32(va));
				break;
			case LM_h:
				snprintf(tmp, sizeof(tmp), spec, (int)(short)u_va_i32(va));
				break;
			default:
				snprintf(tmp, sizeof(tmp), spec, u_va_i32(va));
				break;
			}
			fb_write(&fb, tmp, strlen(tmp));
			sn = save;
			break;
		}
		case 'u':
		case 'o':
		case 'x':
		case 'X': {
			size_t save = sn;
			sputs(spec, sizeof(spec), &sn, (char[2]){real_conv, 0});
			switch (lm) {
			case LM_ll:
				snprintf(tmp, sizeof(tmp), spec, (unsigned long long)u_va_u64(va));
				break;
			case LM_l:
				snprintf(tmp, sizeof(tmp), spec, (unsigned long)u_va_u32(va));
				break;
			case LM_j:
				snprintf(tmp, sizeof(tmp), spec, (uintmax_t)u_va_u64(va));
				break;
			case LM_z:
			case LM_t:
				snprintf(tmp, sizeof(tmp), spec, (size_t)u_va_u32(va));
				break;
			case LM_hh:
				snprintf(tmp, sizeof(tmp), spec, (unsigned int)(unsigned char)u_va_u32(va));
				break;
			case LM_h:
				snprintf(tmp, sizeof(tmp), spec, (unsigned int)(unsigned short)u_va_u32(va));
				break;
			default:
				snprintf(tmp, sizeof(tmp), spec, (unsigned int)u_va_u32(va));
				break;
			}
			fb_write(&fb, tmp, strlen(tmp));
			sn = save;
			break;
		}
		case 'p': { /* 已在上面改写为 x，这里不会到达 */
			break;
		}
		case 'c': {
			unsigned char ch = (unsigned char)(u_va_i32(va) & 0xFF);
			int w = (width > 1) ? width : 1;
			int pad = w - 1;
			if (left) {
				fb_write(&fb, (const char *)&ch, 1);
				fb_pad(&fb, ' ', pad);
			} else {
				fb_pad(&fb, ' ', pad);
				fb_write(&fb, (const char *)&ch, 1);
			}
			break;
		}
		case 's': {
			uint32_t sptr = u_va_u32(va);
			uint32_t read_cap =
				(prec >= 0 && (uint32_t)prec < U_FMT_STR_CAP) ? (uint32_t)prec : U_FMT_STR_CAP;
			uint32_t slen = sptr ? u_strnlen(uc, sptr, read_cap) : 0;

			char sbuf[256];
			char *s = sbuf;
			if (slen + 1 > sizeof(sbuf)) {
				s = (char *)malloc((size_t)slen + 1);
				if (!s) {
					s = sbuf;
					slen = 0;
				}
			}
			if (slen)
				u_read(uc, sptr, s, slen);
			s[slen] = '\0';

			/* 已按精度截断：宽度优先于精度，先截断后填充（标准语义） */
			if (prec >= 0 && (uint32_t)prec < slen)
				slen = (uint32_t)prec;

			int pad = (width > (int)slen) ? (width - (int)slen) : 0;
			if (left) {
				fb_write(&fb, s, slen);
				fb_pad(&fb, ' ', pad);
			} else {
				fb_pad(&fb, ' ', pad);
				fb_write(&fb, s, slen);
			}
			if (s != sbuf)
				free(s);
			break;
		}
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
		case 'a':
		case 'A': {
			size_t save = sn;
			sputs(spec, sizeof(spec), &sn, (char[2]){real_conv, 0});
			if (lm == LM_L) {
				long double v = (long double)u_va_f64(va);
				snprintf(tmp, sizeof(tmp), spec, v);
			} else {
				double v = u_va_f64(va);
				snprintf(tmp, sizeof(tmp), spec, v);
			}
			fb_write(&fb, tmp, strlen(tmp));
			sn = save;
			break;
		}
		case 'n': {
			uint32_t nptr = u_va_u32(va);
			if (nptr) {
				switch (lm) {
				case LM_hh:
					u_wr8(uc, nptr, (uint8_t)(unsigned)fb.total);
					break;
				case LM_h:
					u_wr16(uc, nptr, (uint16_t)(unsigned)fb.total);
					break;
				case LM_ll: {
					uint64_t v = (uint64_t)(unsigned)fb.total;
					u_wr32(uc, nptr, (uint32_t)(v & 0xFFFFFFFFu));
					u_wr32(uc, nptr + 4, (uint32_t)(v >> 32));
					break;
				}
				default:
					u_wr32(uc, nptr, (uint32_t)fb.total);
					break;
				}
			}
			break;
		}
		default:
			/* 未知转换（含 %S/%C 等宽字符扩展）：原样输出，避免静默吞字符 */
			fb_write(&fb, "%", 1);
			fb_write(&fb, &conv, 1);
			break;
		}
	}

	fb_finish(&fb);
	return fb.total;
}

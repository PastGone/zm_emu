#include "zm_file_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* opendir/readdir：找 *.app 主文件名。
 * Windows/MSVC 没有内建 dirent.h，由 xmake 的 `dirent` 包（tronkko/dirent，
 * 纯头文件）提供；类 Unix 用系统自带。两个平台都是同一个 <dirent.h>。 */
#include <dirent.h>
#if defined(_WIN32)
	#include <io.h> /* _access：Windows 没有 unistd.h */
	#ifndef F_OK
		#define F_OK 0
	#endif
	#define access(p, m) _access((p), (m)) /* access/F_OK（TestFile 存在性检查） */
#else
	#include <unistd.h> /* access/F_OK（TestFile 存在性检查） */
#endif
/* GBK→UTF-8 的编解码不属于标准 C：POSIX 侧用 libiconv，Windows 侧改用系统
 * 自带的 CP936 编解码（见下面 gbk_to_utf8 的两套实现）。
 * ★ MinGW **不自带 iconv.h**（要单独装 libiconv），所以这里必须分平台，
 *   否则纯 MinGW 环境下第一行就编不过。 */
#if defined(_WIN32)
	#define WIN32_LEAN_AND_MEAN /* 只要基础 API；避开 winsock1 与其它头的冲突 */
	#include <direct.h>			/* _mkdir：IFileMgr+0x14 建目录 */
	#include <windows.h>		/* MultiByteToWideChar / WideCharToMultiByte（CP936） */
	#define mkdir(p, m) _mkdir(p)
#else
	#include <iconv.h>	  /* GBK→UTF-8（applet 文件名为固件 GBK 编码） */
	#include <sys/stat.h> /* mkdir：IFileMgr+0x14 建目录 */
	#include <sys/types.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
	#include <sys/statvfs.h> /* statvfs：IFileMgr+0x34 查宿主可用空间 */
#endif

#include "../../emu.h"
#include "../../log/log.h"
#include "../core/zm_str.h" /* zm_read_str_obj */
#include "zm_file.h"		/* g_file_data / g_file_size / g_file_pos 状态 */

/* ========== 通用单文件系统：G_FileMgr_ADDR 层 ==========
 *
 *  - 只同时支持一个打开的文件
 *  - fs.open 会在宿主机磁盘上找到同名文件并读入内存，返回固定句柄 FILE1
 *  - 若再次调用 fs.open，会自动关闭前一个文件并打开新文件
 *  - 不需要预先注册任何文件
 *
 * 使用方式：
 *   1. 调用 zm_fs_set_data_dir("/path/to/applet") 设置文件搜索根目录
 *   2. applet 调用 fs.open("xxx") 时，会尝试打开 "/path/to/applet/xxx"
 *   3. 调用 fs.close 或 shutdown 时释放内存
 */

/* 文件查找根目录 */
static char s_data_dir[1024] = {0};

/* ---- 全路径拼接缓冲的容量（一个常量管到底，别再各写各的）----
 *
 * 拼接式样与最大长度：
 *   "%s%s"            → s_data_dir(≤1023) + rel(≤511) + NUL = 1535
 *   "%s/app_list%s"   → 1023 + 9("/app_list") + 511 + NUL  = 1544
 *   "%s/%s%s"         → 1023 + 1 + stem(≤63) + name + NUL  ≈ 1600
 * 以前这里写 1280（也有人写 1536）—— **小于最大可能长度**，于是 GCC 在
 * -Wformat-truncation 下直接告警；Windows 工具链（MinGW/MSVC 的等价检查）
 * 会把它升级成 error，编译直接失败：
 *   warning: '%s' directive output may be truncated writing up to 511 bytes
 *            into a region of size between 257 and 1280 [-Wformat-truncation=]
 * 取 2048：覆盖上面全部式样并留足余量。改这里一处即可，下面所有
 * full / full_path / alt / found 都用它。 */
#define ZM_FULL_PATH_MAX 2048

/* ========== 内部工具 ========== */
static void read_filename_obj(uc_engine *uc,
							  uint32_t ptr,
							  char *buf,
							  size_t cap); /* 定义在下面（TestFile 用的那个） */

/* "这看起来像个文件名吗"：非空 + 每个字节都是可打印 ASCII。
 *
 * 为什么要这个判据：applet 传进来的名字既可能是裸 C 串、也可能是 zmaee 字符串
 * 对象（+0=data_ptr、+4=len），两种形态靠启发式区分（见 zm_read_str_obj）。
 * 一旦判定退化，就会**把对象的指针字段字节当成文件名**读出来 —— 指针字节里
 * 必然出现 0x00（提前截断）或 >=0x80 的字节，所以这个判据能稳定识别"读到的是
 * 指针而不是文本"。
 *
 * 实测（0000042f《落井下石》）：传的是字符串对象 {data_ptr=0x17FB45,…}，启发式
 * 没认出 → 裸读对象首字节 → 名字成了 "E\xFB\x17"（正是 0x17FB45 的小端字节），
 * 于是路径拼错、后续 applet 把字符串当对象用而崩在 0x66E8。 */
static int name_plausible(const char *s) {
	if (!s || !s[0])
		return 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++)
		if (*p < 0x20 || *p >= 0x80)
			return 0;
	return 1;
}

/* 读文件名：**多策略 + 挑第一个"像文件名"的结果**。
 *   ① 原有启发式（对象/裸串二选一，见 zm_read_str_obj）
 *   ② 对象形态：解引用 +0 的 data_ptr（read_filename_obj，TestFile 也用它）
 * ①的结果不可信时才用②，行为对既有 applet 完全不变。 */
static void read_filename(uc_engine *uc, uint32_t ptr, char *buf, size_t cap) {
	buf[0] = '\0';
	zm_read_str_obj(uc, ptr, buf, cap);
	if (name_plausible(buf))
		return;
	char alt[512];
	alt[0] = '\0';
	read_filename_obj(uc, ptr, alt, sizeof(alt));
	if (name_plausible(alt)) {
		snprintf(buf, cap, "%s", alt);
		log_info("fs.open 名字回退：启发式读到非文本字节 → 按字符串对象解引用得 \"%s\"", buf);
	}
}

/* 在目录树里递归找一个**文件名完全匹配 tail** 的文件（深度受限）。
 * 用于"applet 的路径前缀缺失/脏"时的兼容回退（见 zm_fileMgr_open_file）。 */
static int find_file_rec(const char *dir, const char *tail, int depth, char *out, size_t out_cap) {
	if (depth < 0)
		return 0;
	DIR *d = opendir(dir);
	if (!d)
		return 0;
	struct dirent *e;
	int ok = 0;
	while (!ok && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		size_t tn = strlen(tail), dn = strlen(e->d_name);
		int hit = 0;
		if (!strcmp(e->d_name, tail))
			hit = 1;
		else if (tn > 3 && dn > 3) {
			/* 脏前缀会吃掉/多出几个字节 → 允许互为后缀 */
			if (tn <= dn && !strcmp(e->d_name + (dn - tn), tail))
				hit = 1;
			else if (dn <= tn && !strcmp(tail + (tn - dn), e->d_name))
				hit = 1;
		}
		if (hit) {
			snprintf(out, out_cap, "%s/%s", dir, e->d_name);
			ok = 1;
			break;
		}
		if (depth > 0 && e->d_name[0] != '.') {
			char sub[1280];
			snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
			DIR *sd = opendir(sub);
			if (sd) {
				closedir(sd);
				if (find_file_rec(sub, tail, depth - 1, out, out_cap)) {
					ok = 1;
					break;
				}
			}
		}
	}
	closedir(d);
	return ok;
}

/* ASCII 大小写不敏感比较（自己实现，避免 strcasecmp / _stricmp 的平台差异）。 */
static int name_ieq(const char *a, const char *b) {
	for (; *a && *b; a++, b++) {
		unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
		if (ca >= 'A' && ca <= 'Z')
			ca = (unsigned char)(ca + 32);
		if (cb >= 'A' && cb <= 'Z')
			cb = (unsigned char)(cb + 32);
		if (ca != cb)
			return 0;
	}
	return *a == '\0' && *b == '\0';
}

/* 大小写不敏感回退：在**同一个目录**里找只差大小写的同名文件。
 *
 * 为什么需要：applet 的资源包是从 Windows 世界来的（Windows 文件系统不区分
 * 大小写），而仓库里的文件名沿用了原始大小写 → 在 Linux/macOS 这类**区分大小写**
 * 的系统上，applet 报的 "c:\\0000042f.zmr" 与磁盘上的 "0000042F.zmr" 对不上，
 * 直接"找不到文件"。实测（0000042f《落井下石》）：资源包打不开 → 只跑了 60 次槽
 * 调用就崩在自己的错误表里；把文件按小写补一份后立刻变成 **998 次调用、资源全部
 * 加载成功**（然后又暴露了下一个问题）。
 *
 * 只在同一目录内按名字扫描（不递归），命中后把真实路径写回 out（写文件时要用
 * 它，否则自动存档会写到一个新的大写/小写文件上）。 */
static FILE *fopen_ci(const char *path, char *out, size_t out_cap) {
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	char dir[ZM_FULL_PATH_MAX];
	if (slash) {
		size_t n = (size_t)(slash - path);
		if (n >= sizeof(dir))
			return NULL;
		memcpy(dir, path, n);
		dir[n] = '\0';
	} else {
		snprintf(dir, sizeof(dir), ".");
	}
	DIR *d = opendir(dir);
	if (!d)
		return NULL;
	struct dirent *e;
	FILE *fp = NULL;
	while ((e = readdir(d))) {
		if (!name_ieq(e->d_name, base))
			continue;
		snprintf(out, out_cap, "%s/%s", dir, e->d_name);
		fp = fopen(out, "rb");
		break;
	}
	closedir(d);
	return fp;
}

/* GBK→UTF-8（applet 的中文文件名为固件 GBK 编码，宿主文件系统是
 * UTF-8；找不到时做一次转换回退）。成功返转换后长度（不含 NUL），失败返 -1。
 *
 * 两套实现，语义一致：
 *   - POSIX：libiconv 的 iconv_open("UTF-8","GBK") + iconv；
 *   - Windows：系统自带的代码页转换（CP936=GBK → CP_UTF8）。
 * ★ 为什么必须分平台：iconv 不是标准 C，MinGW 不自带 <iconv.h>（要装
 *   libiconv）、MSVC 更没有；而 Windows API 本身就带 CP936 编解码，
 *   用 MultiByteToWideChar + WideCharToMultiByte 零外部依赖。 */
#if defined(_WIN32)
static int gbk_to_utf8(const char *in, char *out, size_t out_cap) {
	if (!in || !out || out_cap < 2)
		return -1;
	const UINT cp_gbk = 936; /* CP936 = GBK */
	int wlen = MultiByteToWideChar(cp_gbk, 0, in, -1, NULL, 0);
	if (wlen <= 0)
		return -1;
	wchar_t *wb = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
	if (!wb)
		return -1;
	if (MultiByteToWideChar(cp_gbk, 0, in, -1, wb, wlen) <= 0) {
		free(wb);
		return -1;
	}
	/* -1 表示连结尾 NUL 一起转 → 返回长度要减 1，与 iconv 版语义对齐 */
	int ulen = WideCharToMultiByte(CP_UTF8, 0, wb, -1, out, (int)out_cap, NULL, NULL);
	free(wb);
	if (ulen <= 0)
		return -1;
	return ulen - 1;
}
#else
static int gbk_to_utf8(const char *in, char *out, size_t out_cap) {
	iconv_t cd = iconv_open("UTF-8", "GBK");
	if (cd == (iconv_t)-1)
		return -1;
	char *src = (char *)in, *dst = out;
	size_t slen = strlen(in), dleft = out_cap - 1;
	size_t r = iconv(cd, &src, &slen, &dst, &dleft);
	iconv_close(cd);
	if (r == (size_t)-1)
		return -1;
	*dst = '\0';
	return (int)(dst - out);
}
#endif

/* 判断字符串是否含高位字节（可能是 GBK 中文） */
static int has_high_byte(const char *s) {
	for (; *s; s++)
		if ((unsigned char)*s >= 0x80)
			return 1;
	return 0;
}

/* ========== ConvertFileName 宿主版（RE：ZMAEE_IFileMgr_ConvertFileName）==========
 * 固件流程：归一化（'\'→'/'、解析 ./ 与 ../、压缩分隔符）→ 按盘符分类：
 *   c:/e: → 内置盘（Android 变体映射 /data/data/<pkg>/...，返 1）
 *   其它盘 → 外置卡（Android 变体映射 GetSdcardPath()/mnt/sdcard，返 2；
 *             含 "assets.zip" 返 3 → 走 zip 包测试链
 *             TestPkgItem/TestPkgItemEx + sub_2A75C 缓存句柄）
 * 注意：RE 来自 Android 移植变体——接口同构但存储映射是安卓专属，
 * 模拟器按宿主语义等价实现：所有盘统一映射到 applet 数据目录，
 * 不照搬 /data/data、/mnt/sdcard 等路径。
 * 返回：1=内置盘 2=外置卡 3=assets.zip -1=非法（归一化后为空）。
 * out 收到不含盘符的相对路径（以 '/' 开头）。 */
static int convert_file_name(const char *in, char *out, size_t cap) {
	const char *p = in;
	int cls = 2; /* 无盘符 → 按外置卡语义（数据目录） */
	if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
		char d = (char)(p[0] | 0x20);
		cls = (d == 'c' || d == 'e') ? 1 : 2;
		p += 2;
	}
	/* 组件栈解析（RE 同款语义）：记录每段在 out 中的起始偏移，
	 * ".." 弹栈、"." 跳过、分隔符压缩 */
	size_t off[64];
	int n = 0;
	size_t o = 0;
	out[0] = '\0';
	while (*p) {
		while (*p == '/' || *p == '\\')
			p++;
		if (!*p)
			break;
		if (p[0] == '.') {
			if (p[1] == '/' || p[1] == '\\' || p[1] == '\0') {
				p++;
				continue;
			}
			if (p[1] == '.' && (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
				p += 2;
				if (n > 0) {
					o = off[--n];
					out[o] = '\0';
				}
				continue;
			}
		}
		if (n >= 64)
			return -1;
		off[n++] = o;
		if (o + 1 < cap)
			out[o++] = '/';
		while (*p && *p != '/' && *p != '\\') {
			if (o + 1 < cap)
				out[o++] = *p;
			p++;
		}
	}
	out[o] = '\0';
	if (o == 0)
		return -1;
	if (strstr(out, "assets.zip"))
		return 3;
	return cls;
}

/* TestFile 专用：按 zmaee 字符串对象布局稳健读取文件名。
 * 固件布局：+0 data_ptr、+12 起内联缓冲（str_assign/str_ctor 同款）。
 * 顺序：解引用 data_ptr → 直接读 ptr+12 → 裸读 ptr。
 * 不复用 zm_read_str_obj 的启发式——内联区首字节为 0 时它会回退到
 * 裸读，把 +0 的指针字节误当文本（00001b62 实测产出"塞"）。 */
static void read_filename_obj(uc_engine *uc, uint32_t ptr, char *buf, size_t cap) {
	buf[0] = '\0';
	if (ptr == 0)
		return;

	/* ★ 先试**内联短串**形态：整串就存在对象头里（短串优化）。
	 *
	 * 实测 00000462《三国情仇》：TestFile 传进来的名字对象 =
	 *     [0..5] = "c:\00000462.zmr"   ← **整串直接躺在 +0** ✗
	 * 旧代码先把它当"数据指针"解释 → 读飞 → 退到 +12 只拿到 "zmr" ✗ ⇒
	 * applet 认为主数据文件不存在 ⇒ 跳过资源/字体初始化 ⇒ 点"开始游戏"后
	 * 画字时把没初始化的字段当字体指针 → 野读崩溃 ✗。
	 *
	 * 判据：可打印 ASCII + 至少 4 字符 + 含 '.'/'\\'/'/'/':' 之一（文件名特征）。
	 * 用指针当字符串看时几乎必然出现 0x00/>=0x80 字节，所以不会误判既有 applet。 */
	if (!getenv("ZM_NO_INLINE_NAME")) {
		uint8_t raw[64];
		if (uc_mem_read(uc, ptr, raw, sizeof(raw)) == UC_ERR_OK) {
			char tmp[sizeof(raw) + 1];
			memcpy(tmp, raw, sizeof(raw));
			tmp[sizeof(raw)] = '\0';
			size_t n = 0;
			while (n < sizeof(raw) && tmp[n])
				n++;
			if (n >= 4 && n < sizeof(raw) && name_plausible(tmp) &&
				(strchr(tmp, '.') || strchr(tmp, '\\') || strchr(tmp, '/') || strchr(tmp, ':'))) {
				snprintf(buf, cap, "%s", tmp);
				return;
			}
		}
	}

	uint32_t dp = 0;
	if (uc_mem_read(uc, ptr, &dp, 4) == UC_ERR_OK && dp != 0) {
		read_cstr(uc, dp, buf, (int)cap);
	}
	if (buf[0] == '\0') {
		read_cstr(uc, ptr + 12, buf, (int)cap);
	}
	if (buf[0] == '\0') {
		read_cstr(uc, ptr, buf, (int)cap);
	}
}

/* ========== 对外 API ========== */

/**
 * 设置文件搜索根目录（通常为 applet 所在文件夹）。
 * 需要在第一次 fs.open 之前调用。
 */
void zm_fs_set_data_dir(const char *dir) {
	if (dir && dir[0]) {
		strncpy(s_data_dir, dir, sizeof(s_data_dir) - 1);
		s_data_dir[sizeof(s_data_dir) - 1] = '\0';
		/* 去掉末尾斜杠 */
		size_t len = strlen(s_data_dir);
		while (len > 0 && (s_data_dir[len - 1] == '/' || s_data_dir[len - 1] == '\\'))
			s_data_dir[--len] = '\0';
	} else {
		s_data_dir[0] = '\0';
	}
}

/**
 * 打开文件 —— 自动关闭前一个文件，加载新文件到内存，返回 FILE1。
 * 成功返回 FILE1，失败返回 0。
 */
/* IFileMgr 通用 stub（g_filemgr_vtbl 未实现槽） */
uint32_t
zm_fileMgr_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
	(void)uc;
	log_debug("fileMgr stub[0x%X] r0=0x%X r1=0x%X r2=0x%X r3=0x%X", off, r0, r1, r2, r3);
	return 0;
}

/* +0x20 TestFile（RE sub_2A7BC）：检查文件是否存在。
 * 固件流程：ConvertFileName 归一化并分类（见 convert_file_name）——
 *   case 0 → 包内资源表（AndroidAEE_TestPkgItem：zip_open+zip_fopen）
 *   case 3 → assets.zip 打包路径（sub_2A75C 缓存句柄 + TestPkgItemEx）
 *   default → 文件系统 access(name, F_OK)
 * 返回：存在 0；不存在 -1；参数空 -4。
 * 模拟器：convert_file_name 统一映射到数据目录；assets.zip 暂不
 * 支持（返 -1，applet 走外置文件回退）。 */
uint32_t zm_fileMgr_TestFile(uc_engine *uc, uint32_t r0, uint32_t name_ptr) {
	if (r0 == 0 || name_ptr == 0)
		return (uint32_t)-4;
	char name[256];
	read_filename_obj(uc, name_ptr, name, sizeof(name));
	/* 诊断：把"名字对象的真实字节"打出来。起因：00000462《三国情仇》点"开始游戏"
	 * 之后崩在字体查表，而上游是 TestFile 读到的名字**只剩后缀**（"zmdata\kingdom.dat"
	 * → "om.dat"、"c:\00000462.zmr" → "zmr" ✗）⇒ applet 认为主数据文件不存在 ⇒ 跳过
	 * 资源/字体初始化 ⇒ 后面拿没初始化的字段当字体指针 ⇒ 野读崩溃。
	 * 这里把对象的 6 个字 + 两层解引用都打出来，用来确定它的真实布局（是
	 * {ptr,len} 还是 {len,inline} 还是带游标/偏移的形态）。ZM_NO_TF_DBG=1 可关。 */
	if (!getenv("ZM_NO_TF_DBG")) {
		static int tf_n = 0;
		if (tf_n++ < 12) {
			uint32_t w[6] = {0};
			uc_mem_read(uc, name_ptr, w, sizeof(w));
			uint32_t lr = 0;
			uc_reg_read(uc, UC_ARM_REG_LR, &lr);
			char at0[64] = {0};
			if (w[0])
				read_cstr(uc, w[0], at0, sizeof(at0));
			log_info("TestFile 名字对象 @0x%X: [0..5]=%X %X %X %X %X %X lr=0x%X "
					 "读到=\"%s\"  [0]处=\"%s\"",
					 name_ptr,
					 w[0],
					 w[1],
					 w[2],
					 w[3],
					 w[4],
					 w[5],
					 lr,
					 name,
					 at0);
		}
	}
	/* GBK 中文文件名 → UTF-8 回退（固件 GBK / 宿主 UTF-8） */
	char utf[256];
	if (has_high_byte(name) && gbk_to_utf8(name, utf, sizeof(utf)) > 0)
		snprintf(name, sizeof(name), "%s", utf);
	char rel[512];
	int cls = convert_file_name(name, rel, sizeof(rel));
	if (cls < 0) {
		log_info("fileMgr.TestFile(\"%s\") -> -1 (归一化后为空)", name);
		return (uint32_t)-1;
	}
	if (cls == 3) {
		/* assets.zip 打包路径（RE：TestPkgItemEx 走 zip_fopen 成员测试）——
		 * 模拟器暂不支持 zip 资源，返不存在，applet 走外置文件回退 */
		log_info("fileMgr.TestFile(\"%s\") -> -1 (assets.zip 暂不支持)", name);
		return (uint32_t)-1;
	}
	int ok = 0;
	char full[ZM_FULL_PATH_MAX];
	if (s_data_dir[0]) {
		snprintf(full, sizeof(full), "%s%s", s_data_dir, rel);
		if (access(full, F_OK) == 0)
			ok = 1;
		if (!ok) { /* 回退 app_list/ 子目录（与 open_file 一致） */
			snprintf(full, sizeof(full), "%s/app_list%s", s_data_dir, rel);
			if (access(full, F_OK) == 0)
				ok = 1;
		}
	}
	if (!ok && access(rel + 1, F_OK) == 0) /* 相对 CWD（rel 以 '/' 开头） */
		ok = 1;
	log_info("fileMgr.TestFile(\"%s\" cls=%d) -> %d", name, cls, ok ? 0 : -1);
	return ok ? 0 : (uint32_t)-1;
}

/* +0x0C = IFileMgr::GetInfo（RE sub_2A550，参考 ZMAEE 实现）：
 *   int GetInfo(mgr, const char *name, info_out *out)
 *   参数空 → -4；整个 out 结构先 memset 成 0（0x4C 字节）；
 *   目录 → out[0]=2，普通文件 → out[0]=0；
 *   out+4 = 大小(高32)   out+8 = 大小(低32)   out+12 = 文件名（basename）
 *   文件不存在 → -1；成功 → 0
 *
 * 为什么必须实现：00000001 装载 res\sprite\*.zmspx 时先调本槽拿长度，
 * 再用它当 IFile::Read 的长度。以前这里是返回 0 的桩 —— 长度被填 0，
 * Read 读 0 字节（真机 ZMAEE_IFile_Read 对 n=0 同样是返回 0、不拷贝），
 * 于是资源内容一个字节都没进缓冲，后续 sub_15684 解析 zms2 头失败返回 0，
 * 列表项的自表指针变成 0，最终在 sub_15964 里读 [0+0x18] 越界
 * → 崩溃 err=6 pc=0x15994。 */
uint32_t zm_fileMgr_GetInfo(uc_engine *uc, uint32_t r0, uint32_t name_ptr, uint32_t out_ptr) {
	if (r0 == 0 || name_ptr == 0 || out_ptr == 0)
		return (uint32_t)-4;

	/* 与真机一致：先把整个 0x4C 结构清零 */
	{
		static const uint8_t z0[0x4C] = {0};
		uc_mem_write(uc, out_ptr, z0, sizeof(z0));
	}

	char name[256];
	/* ★ 必须用 read_filename（字串），不是 read_filename_obj（带对象头的那种）
	 * —— 这里 applet 传的是 sprintf 出来的纯 C 串，用 obj 版会吃掉首字符
	 * （实测 "xiao_hua_normal.zmspx" 变成 "iao_hua_normal.zmspx" → 找不到
	 * → 大小 0 → Read 读 0 字节 → 资源没装载 → 崩）。 */
	read_filename(uc, name_ptr, name, sizeof(name));
	char utf[256];
	if (has_high_byte(name) && gbk_to_utf8(name, utf, sizeof(utf)) > 0)
		snprintf(name, sizeof(name), "%s", utf);

	/* 取大小：走与 open/read_file 同一套路径解析（数据目录 / app_list 回退） */
	uint8_t *buf = NULL;
	size_t len = 0;
	if (zm_fs_read_file(name, &buf, &len) != 0 || !buf) {
		log_info("fileMgr.GetInfo(\"%s\") -> -1 (找不到, out=0x%X)", name, out_ptr);
		return (uint32_t)-1; /* 不存在 */
	}
	free(buf);

	uint32_t cls = 0; /* 0 = 普通文件 */
	uc_mem_write(uc, out_ptr + 0, &cls, 4);
	uint32_t hi = 0, lo = (uint32_t)len;
	uc_mem_write(uc, out_ptr + 4, &hi, 4);
	uc_mem_write(uc, out_ptr + 8, &lo, 4);

	/* basename（真机 strcpy(a3+12, 最后一个 '/' 之后)） */
	const char *base = name;
	for (const char *p = name; *p; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	uc_mem_write(uc, out_ptr + 12, base, strlen(base) + 1);

	log_info("fileMgr.GetInfo(\"%s\") -> 0 (大小=%u, out=0x%X)", name, (unsigned)len, out_ptr);
	return 0;
}

/* +0x30 存储区支持查询（RE sub_29E40）。
 * 返回 ASCII 盘符代码：67='C'（内置盘）、69='E'、84='T'（SD）。
 * a2>=2 时固件走完整 JNI 链（RE：AndroidAEE_CallIntMethod →
 * AEEJni_GetEnv → FindClass("com/zmapp/aee/AEEJNIBridge") →
 * GetMethodID("isSDCardMounted") → CallIntMethod）向 Java 宿主查询
 * SD 卡挂载状态。模拟器角色即"宿主替身"（同 GetTickCount→SDL_GetTicks
 * 的边界决策），宿主目录即"卡"，恒视为已挂载返 84。 */
uint32_t zm_fileMgr_StorageSupport(uc_engine *uc, uint32_t r0, uint32_t type) {
	if (r0 == 0)
		return 0;
	if (type == 0)
		return 67; /* 'C' 内置盘 */
	if (type == 1)
		return 69; /* 'E' */
	return 84;	   /* 'T' SD 卡（恒挂载） */
}

/* +0x34 = IFileMgr::GetFreeSize（RE sub_29E08，libaee.so.c.txt:46929）：
 *   uint32_t GetFreeSize(this, char drive_letter) → **剩余空间字节数**
 *
 * 固件原文：
 *     if (盘符 != 'E' && 盘符 != 'C' && 盘符 != 'c' && 盘符 != 'e') {
 *       kb = ZMAEE_Android_GetSDCardFreeSize(...);      // SD 卡
 *       ZMAEE_DebugPrint("GetSDCardFreeSize=%dKB\n", kb);
 *       return kb << 10;                                // ← KB → 字节
 *     }
 *     return ZMAEE_Android_GetSystemMemSize(...) << 10; // 内置盘
 *
 * 为什么必须实现：00000442《驱蚊大师》**启动时查一次**（实测该槽命中 1 次，
 * 紧跟在 4 次 +0x30 StorageSupport 之后），拿返回值与 100K 比较，不足就弹
 *   "磁盘空间检查 / 你的磁盘空间不足 100K，请整理后再启动此应用程序。"
 * 我们以前是空桩（a_off_fm_34 恒返 0）→ 每次都被判成空间不足 → 必然弹框，
 * 得等它自己超时消失才能进菜单。
 *
 * 宿主替身策略（与 +0x30 StorageSupport 同一个决策）：报宿主**真实可用空间**
 * —— applet 的"卡"就是宿主数据目录，用 statvfs 查该分区。
 * 取不到时退回 128MB，保证任何 applet 的空间门槛都能过。
 *
 * 单位提醒：返回值就是字节数（固件那边的 `<<10` 是它自己 KB→字节的换算，
 * 我们直接给最终值；applet 只拿去比较，不会再移位）。 */
uint32_t zm_fileMgr_GetFreeSize(uc_engine *uc, uint32_t r0, uint32_t drive) {
	(void)uc;
	if (r0 == 0)
		return 0;

	uint32_t freeb = 0;
	/* ★ 不要再拿**宿主磁盘**的可用空间 ✗✗
	 *
	 * 以前是 statvfs(数据目录)，本机剩几百 GB → 夹到 2GB 交给 applet。后果实测
	 * （0000050b《新还猪格格》，同一族都中招）：
	 *   applet 按这个"剩余空间"估算缓冲 → `u_heap: 内存不足，malloc(16581376) 失败`
	 *   （15.8MB ✗，就来自那个 2GB ✗）→ 拿到 NULL 之后它把**尺寸值**当指针用
	 *   → 后面所有写地址都带着 0xFD1000 的影子（0xFD1000 / 0x1011000 / 0x1FD1000）
	 *   → err=7 写到映射外 ✓✓。
	 *
	 * 真机上这是**闪存卡容量**（掌盟年代几十 MB～GB 级），是个"像设备"的数，
	 * 不是宿主的磁盘。默认给 64MB（可用 ZM_FREE_MB 调）；这样 applet 的按比例估算
	 * 会落在我们 6MB 的 u_heap 之内，分配成功、不会退化成"拿尺寸当指针"。 */
	{
		const char *e = getenv("ZM_FREE_MB");
		uint32_t mb = (e && atoi(e) > 0) ? (uint32_t)atoi(e) : 64u;
		freeb = mb * 1024u * 1024u;
	}

	log_info("fileMgr.GetFreeSize(盘符 '%c'=0x%X) -> %u 字节（%.1f MB）",
			 (drive >= 0x20 && drive < 0x7F) ? (char)drive : '?',
			 drive,
			 freeb,
			 (double)freeb / 1048576.0);
	return freeb;
}

/* ★ 共享数据目录映射：guest 的框架数据根 → 仓库的 **applet/data/**。
 *
 * 【guest 侧的真实路径】applet 模板是 `e:\zmol\zmdata\qblox.dat`（字面量，
 * 首字节被盘符覆盖），但它的路径拼装函数会把它**重建成框架形态**：
 *       盘符:  +  `\zmaee\data\`  +  模板 +8 起（"zmdata\qblox.dat"）
 * 实测本 applet 最终请求的就是：
 *       "E:\zmaee\data\zmdata\qblox.dat"      ← 日志里的原始名字
 * （`\zmol\` 那份常量只用于"判断模板形态"，`\zmaee\data\` 才是落点。）
 *
 * 【仓库侧的既有约定】applet/data/zmdata/ 下已经放着 castlev/、huarongdao/
 * 两个占位目录 —— 就是给这些 guest 数据文件用的。所以：
 *       /zmaee/data/zmdata/qblox.dat  →  <仓库>/applet/data/zmdata/qblox.dat
 *       /zmol/zmdata/...              →  同样落到 applet/data/...（保险，另一形态）
 * 改动前落到 <applet 自己的目录>/zmaee/data/... ✗ —— 与仓库约定不符。
 *
 * 资源类路径（"c:\0000042f.zmr" → "/0000042f.zmr"）不以这两个根开头，不受影响。 */
static const char *map_shared_data(const char *rel, char *out, size_t cap) {
	const char *tail = NULL;
	if (strncmp(rel, "/zmaee/data", 11) == 0 && (rel[11] == '\0' || rel[11] == '/'))
		tail = rel + 11;
	else if (strncmp(rel, "/zmol", 5) == 0 && (rel[5] == '\0' || rel[5] == '/'))
		tail = rel + 5;
	/* **相对形态**：applet 直接拿 "zmdata\\qblox.dat" 当名字（不带盘符/框架前缀）。
	 * 实测 0000042f（存档）就是这么请求的 —— 以前这条落进 applet 自己的目录，
	 * 于是：
	 *   · 存档写到了 <applet 目录>/zmdata/qblox.dat（仓库里那 30 个空 zmdata/、
	 *     52 个空 zmaee/ 残留就是这么来的）；
	 *   · 而仓库约定这些公共数据放在 **applet/data/**（E:\zmaee\data\ 的宿主侧，
	 *     里面已经有 font/ zmimages/ castlev/ huarongdao/ 等 21 个公共数据包）。
	 * 这里把 "zmdata/..." 整段当 tail —— 结果就是 <repo>/applet/data/zmdata/...，
	 * 与另外两种形态（/zmaee/data/…、/zmol/…）**落到同一个地方** ✓。 */
	else if (strncmp(rel, "/zmdata", 7) == 0 && (rel[7] == '\0' || rel[7] == '/'))
		tail = rel; /* 注意：整段都算 tail（它自己就含 zmdata 这一级） */
	if (!tail)
		return rel; /* 不是框架数据根 → 原样返回 */
	char parent[ZM_FULL_PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", s_data_dir);
	char *slash = strrchr(parent, '/');
	if (!slash)
		return rel;
	*slash = '\0'; /* <...>/applet （applet 目录的上一级） */
	snprintf(out, cap, "%s/data%s", parent, tail);
	return out;
}

/* ==========================================================================
 * 目录创建（IFileMgr +0x14）与"新建文件"判定
 *
 * 【RE 定案】+0x14 = mkdir（确保目录存在）。两份反编译对照：
 *   安卓 res/安卓落井下石/0000dc5e.aso.c  sub_19720(...)：
 *     zmaee_strlen/strcpy → 逐级把路径里的 '/'、'\\' 换成 0 →
 *     (*(vt + 20))(fm, 部分路径) → 还原分隔符
 *     —— 就是"逐级建目录"；读/存档前都会先调它（见 zmold_fopen 的 'w' / '+' 分支）
 *   手机版 applet/0000042f 0x111AC 同款：
 *     0x11210  ldr r0,[r5]          ; r0 = FileMgr
 *     0x11218  ldr r2,[r0,#0x14]    ; ★ vt[0x14]
 *     0x11220  blx r2
 *
 * 【为什么要记一笔】真机上 mkdir 之后 OpenFile 会**新建**文件（存档 8 字节就是
 * 这么落下去的）。宿主文件得由我们自己创建，于是：
 *   - 本槽在宿主上真的把目录建出来（mkdir -p），并把宿主全路径记进 s_made_dirs；
 *   - OpenFile 找不到文件、但它的**父目录刚被 applet 建过** → 按"新建空文件"处理，
 *     给 0 字节缓冲 + 记住宿主路径，随后 Write 扩容、Close 写回（自动存档闭环）。
 * ========================================================================== */
#define ZM_MADE_DIR_MAX 16
static char s_made_dirs[ZM_MADE_DIR_MAX][ZM_FULL_PATH_MAX];
static int s_made_dir_n = 0;

static int dir_was_made(const char *host_dir) {
	for (int i = 0; i < s_made_dir_n; i++)
		if (!strcmp(s_made_dirs[i], host_dir))
			return 1;
	return 0;
}

static void remember_made_dir(const char *host_dir) {
	for (int i = 0; i < s_made_dir_n; i++)
		if (!strcmp(s_made_dirs[i], host_dir))
			return; /* 已记过 */
	if (s_made_dir_n < ZM_MADE_DIR_MAX)
		snprintf(s_made_dirs[s_made_dir_n++], ZM_FULL_PATH_MAX, "%s", host_dir);
}

/* mkdir -p 的等价物（宿主目录） */
static void mkdir_p(const char *path) {
	char tmp[ZM_FULL_PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

/* IFileMgr +0x14：建目录。参数同 OpenFile：r0 = FileMgr 对象、r1 = guest 路径
 * （applet 传的是**逐级拆分后的部分路径**，如 "E:"、"E:\zmol"、"E:\zmol\zmdata"）。 */
uint32_t zm_fileMgr_make_dir(uc_engine *uc, uint32_t path_ptr) {
	if (!path_ptr || !s_data_dir[0])
		return 0;
	char name[512];
	name[0] = '\0';
	read_filename(uc, path_ptr, name, sizeof(name));
	if (!name[0])
		return 0;
	if (has_high_byte(name)) { /* 与 open 同款：GBK → UTF-8 */
		char utf[512];
		if (gbk_to_utf8(name, utf, sizeof(utf)) > 0)
			snprintf(name, sizeof(name), "%s", utf);
	}
	char rel[512];
	if (convert_file_name(name, rel, sizeof(rel)) < 0)
		return 0; /* 例如纯盘符 "E:"：没有目录段，无事可做 */
	char full[ZM_FULL_PATH_MAX];
	char mapped[ZM_FULL_PATH_MAX];
	if (map_shared_data(rel, mapped, sizeof(mapped)) != rel)
		snprintf(full, sizeof(full), "%s", mapped);
	else
		snprintf(full, sizeof(full), "%s%s", s_data_dir, rel);
	mkdir_p(full);
	remember_made_dir(full);
	log_info("[MKDIR] \"%s\" → %s", name, full);
	return 0;
}

uint32_t zm_fileMgr_open_file(uc_engine *uc, uint32_t filename_ptr) {
	if (getenv("ZM_LOG_OPEN")) {
		uint8_t raw[48] = {0};
		uc_mem_read(uc, filename_ptr, raw, 47);
		uint32_t lr = 0;
		uc_reg_read(uc, UC_ARM_REG_LR, &lr);
		uint32_t f0 = 0, f1 = 0, f2 = 0, f3 = 0;
		uc_mem_read(uc, filename_ptr, &f0, 4);
		uc_mem_read(uc, filename_ptr + 4, &f1, 4);
		uc_mem_read(uc, filename_ptr + 8, &f2, 4);
		uc_mem_read(uc, filename_ptr + 12, &f3, 4);
		char at0[64] = {0}, at1[64] = {0};
		if (f0)
			uc_mem_read(uc, f0, at0, 63);
		if (f1)
			uc_mem_read(uc, f1, at1, 63);
		log_info("[OPEN] ptr=0x%X lr=0x%X raw=\"%s\"", filename_ptr, lr, (char *)raw);
		log_info(
			"[OPEN]  fields: [0]=0x%X [4]=0x%X [8]=0x%X [12]=0x%X | str@[0]=\"%s\" str@[4]=\"%s\"",
			f0,
			f1,
			f2,
			f3,
			at0,
			at1);
		char hx[3 * 32 + 1];
		for (int k = 0; k < 32; k++)
			snprintf(hx + k * 3, 4, "%02X ", raw[k]);
		log_info("[OPEN]  对象 32 字节: %s", hx);
		if (f0) {
			uint8_t b[32] = {0};
			char h2[3 * 32 + 1];
			uc_mem_read(uc, f0 - 4, b, 32);
			for (int k = 0; k < 32; k++)
				snprintf(h2 + k * 3, 4, "%02X ", b[k]);
			log_info("[OPEN]  data-4 处 32 字节: %s", h2);
		}
	}

	(void)uc;
	char name[256];
	read_filename(uc, filename_ptr, name, sizeof(name));
	/* ★ applet 的文件名是固件 **GBK** 编码，宿主文件系统是 UTF-8 —— 必须转换。
	 * 这条路径以前漏了 ✗（同文件里 GetInfo/TestFile/read_file 都有转换 ✓），
	 * 于是 00000502 的资源加载阶段路径前缀乱码、找不到 0005.rms / info.dat ✗。 */
	if (has_high_byte(name)) {
		char utf[512];
		if (gbk_to_utf8(name, utf, sizeof(utf)) > 0)
			snprintf(name, sizeof(name), "%s", utf);
	}
	char rel[512];
	int cls = convert_file_name(name, rel, sizeof(rel));
	if (cls < 0) {
		log_warn("fs.open(\"%s\") -> 0 (归一化后为空)", name);
		return 0;
	}

	/* A/B 开关：排查"某个改动让 applet 从能跑变不能跑"时用。
	 * 置 1 → 回到**旧行为**：在尝试打开之前就 free 掉上一个文件（失败时 applet
	 * 之后读到的就是 0，而不是"上一个文件的内容"）。 */
	if (getenv("ZM_OPEN_EAGER_CLOSE") && g_file_data) {
		free(g_file_data);
		g_file_data = NULL;
		g_file_size = 0;
		g_file_pos = 0;
	}

	/* 【不要在"尝试打开"之前就关掉旧文件】——这里原来是：
	 *     if (g_file_data) { free(g_file_data); ... = NULL; }
	 * 于是**打开失败也会把上一个文件弄没** ✗。而 applet 的常规流程里到处是
	 * "先试开一个可有可无的存档，失败就继续用手里那个资源包"：
	 *   实测 000004dc《仙剑》：fs.open(".dat")(773KB，ZIP 资源包) → 读目录/解资源
	 *   （500+ 次 Read 全成功）→ fs.open("\info.dat") 首次运行不存在 ✗ →
	 *   旧代码在这里把 .dat 释放了 → 之后所有 Read/Seek/Tell 全返回 0 ✗ →
	 *   它构造出来的"流对象"绑不上数据源（内层接口字段为 0）→ 解引用野指针崩溃 ✗。
	 * 真机上 OpenFile 失败**不会**影响别的文件对象，所以这里改成"加载成功才替换"
	 * （见下面 install 处）。 */

	if (s_data_dir[0] == '\0') {
		log_error("zm_fs_open: 未设置数据目录，无法打开 \"%s\"", name);
		return 0;
	}

	char full_path[ZM_FULL_PATH_MAX];
	{
		/* 共享数据根（X:\zmol\...）→ 仓库 applet/data/...，见 map_shared_data */
		char mapped[ZM_FULL_PATH_MAX];
		if (map_shared_data(rel, mapped, sizeof(mapped)) != rel)
			snprintf(full_path, sizeof(full_path), "%s", mapped);
		else
			snprintf(full_path, sizeof(full_path), "%s%s", s_data_dir, rel);
	}
	/* ★ 退化名回退：当 applet 请求的文件名只剩后缀（如 ".dat"，主文件名为空）时，
	 * 回退到**该 applet 目录下 *.app 的主文件名 + 该后缀**（00000502/ →
	 * 00000502.dat ✓，000004051/ 里是 00000405.app → 00000405.dat ✓）。
	 * 主文件名本该由引擎提供（applet 自己的包/id），我们没给 → 名字里那段是空的；
	 * 这个回退让链路至少能找到它自己的数据文件。实测：00000502 / 000004e9 的
	 * 崩点由 0x80E291E8 前进到 0x17F998（并开始加载 c2sraiden/info.dat 等真实
	 * 资源）。ZM_NO_DATFALLBACK=1 可关闭。 */
	if (!getenv("ZM_NO_DATFALLBACK") && name[0] == '.' && name[1]) {
		char stem[64] = {0};
		DIR *d = opendir(s_data_dir);
		if (d) {
			struct dirent *e;
			while ((e = readdir(d))) {
				const char *dot = strrchr(e->d_name, '.');
				if (dot && !strcmp(dot, ".app")) {
					size_t n = (size_t)(dot - e->d_name);
					if (n >= sizeof(stem))
						n = sizeof(stem) - 1;
					memcpy(stem, e->d_name, n);
					stem[n] = '\0';
					break;
				}
			}
			closedir(d);
		}
		if (!stem[0]) {
			const char *slash = strrchr(s_data_dir, '/');
			snprintf(stem, sizeof(stem), "%s", (slash && slash[1]) ? slash + 1 : "app");
		}
		snprintf(full_path, sizeof(full_path), "%s/%s%s", s_data_dir, stem, name);
		log_info("[FB] 退化名回退: \"%s\" → %s", name, full_path);
	}

	FILE *fp = fopen(full_path, "rb");
	/* 回退：00000405 的 \config.b 实际在 app_list/ 子目录下
	 * （applet 运行工作目录是 app_list）。 */
	if (!fp) {
		char alt[ZM_FULL_PATH_MAX];
		snprintf(alt, sizeof(alt), "%s/app_list%s", s_data_dir, rel);
		fp = fopen(alt, "rb");
		if (fp)
			snprintf(full_path, sizeof(full_path), "%s", alt);
	}
	/* ★ 兼容回退：applet 的路径前缀本该由引擎提供（工作目录），我们这边是空的/
	 * 脏的 → 名字形如 "?n?info.dat"。此时取名字里的**可打印尾部**（最后一个非
	 * 可打印字节之后的部分），到 applet 目录下**递归**找同名文件（深度≤3）。
	 * 实测价值：00000502 要的 c2sraiden/info.dat 就在它自己目录下 ✓。 */
	if (!fp && !getenv("ZM_NO_DEEPFIND")) {
		/* 取**最长的可打印 ASCII 后缀**：脏前缀是高字节（GBK/UTF-8 残留），
		 * 从最后一个非 ASCII 字节之后开始算。 */
		const char *tail = name;
		{
			size_t n = strlen(name);
			while (n > 0 &&
				   ((unsigned char)name[n - 1] >= 0x20 && (unsigned char)name[n - 1] < 0x80))
				n--;
			tail = name + n;
		}
		if (tail != name && *tail) {
			char found[ZM_FULL_PATH_MAX];
			if (find_file_rec(s_data_dir, tail, 3, found, sizeof(found))) {
				snprintf(full_path, sizeof(full_path), "%s", found);
				fp = fopen(full_path, "rb");
				if (fp)
					log_info("[DEEP] 前缀脏 → 递归命中: \"%s\" → %s", name, full_path);
			}
		}
	}
	/* 大小写回退：applet 报的名字与磁盘上只差大小写（Windows 资产在 Linux 上必踩，
	 * 实测 0000042f 的 "0000042f.zmr" vs 磁盘 "0000042F.zmr"）。命中后 full_path
	 * 换成真实路径，后续自动存档才会写回同一个文件。 */
	if (!fp) {
		char ci[ZM_FULL_PATH_MAX];
		fp = fopen_ci(full_path, ci, sizeof(ci));
		if (fp) {
			snprintf(full_path, sizeof(full_path), "%s", ci);
			log_info("[CI] 大小写回退命中: \"%s\" → %s", name, full_path);
		}
	}
	/* 新建文件：applet 的存档流程是 mkdir(逐级目录) → OpenFile → Write → Close
	 * （安卓 zmold_fopen 的 'w' 分支会先调 sub_19720 建目录，手机版 0000042f 同款）。
	 * 宿主上没有这个文件、但它的**父目录刚被 applet 建过** → 按"新建空文件"处理：
	 * 给一个 0 字节缓冲并记住宿主路径，随后的 Write 会扩容、Close 会写回。 */
	if (!fp) {
		char parent[ZM_FULL_PATH_MAX];
		snprintf(parent, sizeof(parent), "%s", full_path);
		char *slash = strrchr(parent, '/');
		if (slash) {
			*slash = '\0';
			if (dir_was_made(parent)) {
				free(g_file_data);					   /* 旧文件在这里才被替换（见上面的说明） */
				g_file_data = (uint8_t *)calloc(1, 1); /* 空文件，Write 时扩容 */
				g_file_size = 0;
				g_file_pos = 0;
				g_file_dirty = 0;
				snprintf(g_file_path, sizeof(g_file_path), "%s", full_path);
				log_info("fs.open(\"%s\") -> FILE1（新建空文件：目录 %s 刚由 applet 建过）",
						 name,
						 parent);
				return FILE1;
			}
		}
	}
	if (!fp) {
		/* 名字脏/短是这类 applet 的常见病（路径前缀本该由引擎提供，我们这边可能是空
		 * 的或脏的）。这里额外打印**原始字节**和**调用者 LR**：一眼区分"GBK/UTF-16
		 * 残留""名字根本没写""被写短了"，LR 还能直接对上反汇编里的调用点。 */
		char hex[3 * 25];
		int hp = 0;
		hex[0] = '\0';
		for (int i = 0; i < 24 && name[i] && hp + 4 < (int)sizeof(hex); i++)
			hp += snprintf(hex + hp, sizeof(hex) - (size_t)hp, "%02X ", (unsigned char)name[i]);
		if (hp > 0)
			hex[hp - 1] = '\0';
		uint32_t lr = 0;
		if (g_uc)
			uc_reg_read(g_uc, UC_ARM_REG_LR, &lr);
		log_warn("zm_fs_open: 找不到文件 \"%s\" (全路径: %s) 原始字节=[%s] 调用者LR=0x%X",
				 name,
				 full_path,
				 hex,
				 lr);
		/* 实验开关：文件不存在时也返回一个"空文件句柄"，用来判定上游那条链
		 * （00000502：FileMgr[8] 的返回值 → 对象 +0x10 → 类表条目就绪标志）
		 * 是不是只差"非 0 返回值"。默认关闭。 */
		if (getenv("ZM_OPEN_DUMMY")) {
			uint8_t *z = (uint8_t *)calloc(1, 1);
			free(g_file_data); /* 替换时才释放旧的 */
			g_file_data = z;
			g_file_size = 0;
			g_file_pos = 0;
			g_file_dirty = 0;
			g_file_path[0] = '\0';
			log_info("fs.open(\"%s\") -> FILE1 (ZM_OPEN_DUMMY 空句柄)", name);
			return FILE1;
		}
		/* A/B 开关：失败时的返回值可配（ZM_OPEN_FAIL_RET=0xffffffff 等）。
		 *
		 * 为什么要能配：applet 对"文件不存在"的判据可能是 `if (h)`（0 表示失败），
		 * 也可能是 `if (h < 0)`（负数表示失败）。我们一向返回 0 —— 对后者来说
		 * **0 会被当成"打开成功"** ✗，于是它拿 0 当句柄/对象继续用。
		 * 实测 00000462《三国情仇》：它会去开 `zmdata\kingdom.dat`（疑似存档/资源），
		 * 拿到的就是 0，之后在游戏初始化里用了一堆没被初始化的对象 → 崩。
		 * 默认仍返回 0（不改既有行为），排查时逐个取值试。 */
		{
			const char *fr = getenv("ZM_OPEN_FAIL_RET");
			if (fr && fr[0]) {
				uint32_t v = (uint32_t)strtoul(fr, NULL, 0);
				log_info("fs.open(\"%s\") -> 0x%X（ZM_OPEN_FAIL_RET）", name, v);
				return v;
			}
		}
		return 0;
	}

	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz < 0) {
		fclose(fp);
		return 0;
	}

	uint8_t *buf = malloc((size_t)sz);
	if (!buf) {
		fclose(fp);
		return 0;
	}
	if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
		free(buf);
		fclose(fp);
		return 0;
	}
	fclose(fp);

	/* 到这里才真正替换上一个文件（加载成功）——见函数上方"不要在尝试打开之前就
	 * 关掉旧文件"的长注释：失败时保持原文件可用，否则 applet 手里的资源包会凭空
	 * 消失，后续读全 0，最后走到野指针崩溃。 */
	free(g_file_data);
	g_file_data = buf;
	g_file_size = (size_t)sz;
	g_file_pos = 0;
	g_file_dirty = 0;
	/* 记住宿主机全路径：Write 之后 close 时要靠它写回（自动存档） */
	snprintf(g_file_path, sizeof(g_file_path), "%s", full_path);

	log_info("fs.open(\"%s\") -> FILE1 (size=%zu)", name, g_file_size);
	return FILE1; /* FILE1 是预定义的虚拟句柄地址 */
}

uint32_t zm_fs_release(uc_engine *uc) {
	(void)uc;
	return 0;
}

/* 宿主侧整文件读取：与 open_file 同一套路径解析（数据目录 / app_list 回退），
 * 但不占用 FILE1 单例句柄——IImage 这类"内部再读一个文件"的场景必须用它，
 * 否则会把 applet 正在解析的文件句柄顶掉。 */
int zm_fs_read_file(const char *name, uint8_t **out_buf, size_t *out_len) {
	if (out_buf)
		*out_buf = NULL;
	if (out_len)
		*out_len = 0;
	if (!name || !name[0] || !out_buf || !out_len)
		return -1;

	char utf[256];
	if (has_high_byte(name) && gbk_to_utf8(name, utf, sizeof(utf)) > 0)
		name = utf;

	char rel[512];
	if (convert_file_name(name, rel, sizeof(rel)) < 0)
		return -1;

	FILE *fp = NULL;
	char full[ZM_FULL_PATH_MAX];
	if (s_data_dir[0]) {
		snprintf(full, sizeof(full), "%s%s", s_data_dir, rel);
		fp = fopen(full, "rb");
		if (!fp) {
			snprintf(full, sizeof(full), "%s/app_list%s", s_data_dir, rel);
			fp = fopen(full, "rb");
		}
	}
	if (!fp) {
		fp = fopen(rel + 1, "rb"); /* 相对 CWD（rel 以 '/' 开头） */
		if (fp)
			snprintf(full, sizeof(full), "%s", rel + 1);
	}
	if (!fp) {
		log_warn("zm_fs_read_file: 找不到文件 \"%s\"", name);
		return -1;
	}

	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz < 0) {
		fclose(fp);
		return -1;
	}
	uint8_t *buf = malloc((size_t)sz ? (size_t)sz : 1);
	if (!buf) {
		fclose(fp);
		return -1;
	}
	if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
		free(buf);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*out_buf = buf;
	*out_len = (size_t)sz;
	log_debug("zm_fs_read_file(\"%s\") -> %zu 字节", name, (size_t)sz);
	return 0;
}

/* ---------- 默认初始化（仅设置数据目录，不扫描任何文件） ---------- */
void zm_fs_register_default(const char *applet_dir) {
	zm_fs_set_data_dir(applet_dir);
	log_info("zm_fs: 数据目录 = \"%s\"（文件将在 fs.open 时按需加载）", s_data_dir);
}

/* ---------- 写回宿主机文件 ----------
 * IFile.Write 改动的内容最终由这里落盘。
 * 00000506 每 10 秒写一次 40 字节的存档 `data`（SetTimer(10000, cb=0x9F84)）；
 * 以前 +0x0C 这个槽没接，applet 的存档/设置从来没能保存。 */
int zm_fs_write_back(const char *full_path, const uint8_t *data, size_t len) {
	if (!full_path || !full_path[0])
		return -1;
	FILE *fp = fopen(full_path, "wb");
	if (!fp) {
		log_error("zm_fs_write_back: 打不开 \"%s\" 写入", full_path);
		return -1;
	}
	if (len && fwrite(data, 1, len, fp) != len) {
		fclose(fp);
		log_error("zm_fs_write_back: 写 \"%s\" 失败", full_path);
		return -1;
	}
	fclose(fp);
	return 0;
}

/* ---------- 写回宿主机文件（**按 applet 给的名字**，找不到就创建）----------
 *
 * 用途：CBK 文件对象（zm_cbk_file.c）关闭时要落盘 —— 那一族的"存档/设置"
 * 就是"打开（或新建）一个文件 → 写 → 关闭"，而它的名字是**固件风格**的
 * （`\save\info.dat` 这种 ✗），需要走和 zm_fs_read_file 同一套归一化。
 *
 * 关键点：**文件不存在也要能写**（新建存档正是这种情况 ✗），所以这里不要求
 * 目标已存在：目录不存在就逐级建（applet 一般会先调 IFileMgr::Mkdir ✓）。
 */
int zm_fs_write_back_name(const char *name, const uint8_t *data, size_t len) {
	if (!name || !name[0])
		return -1;

	char utf[256];
	if (has_high_byte(name) && gbk_to_utf8(name, utf, sizeof(utf)) > 0)
		name = utf;

	char rel[512];
	if (convert_file_name(name, rel, sizeof(rel)) < 0)
		return -1;

	char full[ZM_FULL_PATH_MAX];
	full[0] = '\0';
	if (s_data_dir[0]) {
		snprintf(full, sizeof(full), "%s%s", s_data_dir, rel);
		FILE *t = fopen(full, "rb");
		if (!t) {
			/* 已存在的话优先跟随 app_list 里的同名文件（和读路径保持一致） */
			char alt[ZM_FULL_PATH_MAX];
			snprintf(alt, sizeof(alt), "%s/app_list%s", s_data_dir, rel);
			t = fopen(alt, "rb");
			if (t) {
				fclose(t);
				snprintf(full, sizeof(full), "%s", alt);
			} else {
				/* 新建：把父目录建出来（层层建，忽略已存在） */
				char dir[ZM_FULL_PATH_MAX];
				snprintf(dir, sizeof(dir), "%s%s", s_data_dir, rel);
				for (char *p = dir + strlen(s_data_dir) + 1; *p; p++) {
					if (*p == '/') {
						*p = '\0';
						mkdir(dir, 0755);
						*p = '/';
					}
				}
			}
		} else {
			fclose(t);
		}
	} else {
		snprintf(full, sizeof(full), "%s", rel + 1);
	}

	int r = zm_fs_write_back(full, data, len);
	log_info("fs 写回(按名): \"%s\" → %s（%zu 字节）%s", name, full, len, r == 0 ? "" : " ✗失败");
	return r;
}

/* ---------- 清理 ---------- */
void zm_fs_shutdown(void) {
	if (g_file_data) {
		free(g_file_data);
		g_file_data = NULL;
	}
	g_file_size = 0;
	g_file_pos = 0;
	s_data_dir[0] = '\0';
}

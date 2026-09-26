#include "../log/log.h"
#include <stdio.h>
#include <string.h>

long get_file_size(FILE *fp) {
	fseek(fp, 0, SEEK_END);
	long file_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	return file_size;
}

void pause_console(void) {
	log_info("按回车键继续...");
	scanf("%*c");
}

/**
 * 功能：从完整的路径名中提取出最后的文件名。
 * 示例：输入 "C:\\zmaee\\exec\\app.bin" -> 返回 "app.bin"
 */
const char *get_filename_from_fullpath(const char *full_path) {
	if (full_path == NULL)
		return NULL;

	const char *last_sep = strrchr(full_path, '\\');
	const char *last_sep2 = strrchr(full_path, '/');

	if (last_sep2 > last_sep) {
		last_sep = last_sep2;
	}

	// 如果找到了分隔符，返回分隔符后的内容；否则返回原字符串
	return (last_sep != NULL) ? (last_sep + 1) : full_path;
}

/**
 * 功能：从完整的路径名中提取出父目录路径。
 * 示例：
 *   输入 "C:\\zmaee\\exec\\app.bin"  -> 输出 "C:\\zmaee\\exec\\"
 *   输入 "/system/lib/module.so"     -> 输出 "/system/lib/"
 *   输入 "C:\\app.bin"              -> 输出 "C:\\"
 *   输入 "app.bin"                  -> 输出 "." (表示当前目录)
 *
 * @param full_path   [输入] 完整的文件路径（支持 '/' 或 '\\'）
 * @param out_path    [输出] 提取出的目录路径缓冲区
 * @param out_size    [输入] 输出缓冲区的大小
 * @return 成功返回 out_path 指针，失败返回 NULL
 */
char *get_dir_from_fullpath(const char *full_path, char *out_path, size_t out_size) {
	if (full_path == NULL || out_path == NULL || out_size == 0) {
		return NULL;
	}

	// 1. 安全拷贝到输出缓冲区
	strncpy(out_path, full_path, out_size - 1);
	out_path[out_size - 1] = '\0';

	// 2. 从后往前查找最后一个路径分隔符
	char *last_sep = strrchr(out_path, '\\'); // Windows 风格
	char *last_sep2 = strrchr(out_path, '/'); // Unix 风格

	// 取两者中位置靠后的那一个（处理混用的情况）
	if (last_sep2 > last_sep) {
		last_sep = last_sep2;
	}

	// 3. 如果找到了分隔符
	if (last_sep != NULL) {
		// 检查分隔符后面是否还有内容（即不是以分隔符结尾的纯目录路径）
		if (*(last_sep + 1) != '\0') {
			// 将分隔符的下一位截断，相当于删掉了最后的文件名
			*(last_sep + 1) = '\0';
		}
		// 如果已经是 "C:\\" 或 "/" 这种根目录，则原样保留
	} else {
		// 4. 如果没有任何分隔符（纯文件名），返回当前目录
		if (out_size >= 2) {
			out_path[0] = '.';
			out_path[1] = '\0';
		} else {
			out_path[0] = '\0';
		}
	}

	return out_path;
}
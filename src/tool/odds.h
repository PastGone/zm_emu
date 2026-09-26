#ifndef ODDS_H
#define ODDS_H // 这个单词的意思是零碎杂碎所以这里放一些杂碎的函数
#include <stdio.h>
long get_file_size(FILE *fp);
void pause_console(void);
const char *get_filename_from_fullpath(const char *full_path);
char *get_dir_from_fullpath(const char *full_path, char *out_path, size_t out_size);
#endif
#ifndef TEST_PARSE_H
#define TEST_PARSE_H

/* 解析并打印一个 .app 文件头。
 * path 为 NULL 时从 stdin 读取路径（交互模式）。
 * 成功返回 0。 */
int test_parse(const char *path);

#endif

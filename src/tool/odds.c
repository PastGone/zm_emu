#include "../log/log.h"
#include <stdio.h>

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

#include "test_parse.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_SIZE 0x188 // 头部固定长度

// 安全打印字符串（最多 len 字节，遇到不可打印字符用 '.' 代替）
void print_safe(const unsigned char *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    putchar((data[i] >= 0x20 && data[i] <= 0x7E) ? data[i] : '.');
  }
}

int test_parse(const char *path) {
  char filename[512];

  if (path && *path) {
    snprintf(filename, sizeof(filename), "%s", path);
  } else {
    printf("请输入 app 文件路径: ");
    if (!fgets(filename, sizeof(filename), stdin)) {
      fprintf(stderr, "未读到文件路径\n");
      return 1;
    }
    filename[strcspn(filename, "\n")] = '\0';
  }

  FILE *fp = fopen(filename, "rb");
  if (!fp) {
    perror("打开文件失败");
    return 1;
  }

  unsigned char buf[HEADER_SIZE];
  size_t read_bytes = fread(buf, 1, HEADER_SIZE, fp);
  if (read_bytes < HEADER_SIZE) {
    fprintf(stderr, "文件太小，无法读取完整头部 (需要 %d 字节)\n", HEADER_SIZE);
    fclose(fp);
    return 1;
  }

// ---- 解析多字节整数字段（小端序） ----
#define GET_U32(off)                                                           \
  (buf[off] | buf[off + 1] << 8 | buf[off + 2] << 16 | buf[off + 3] << 24)

  uint32_t applet_id = GET_U32(0x00);
  uint32_t version = GET_U32(0x04);
  uint32_t flags = GET_U32(0x08);
  uint32_t payload_size = GET_U32(0x0C);
  // 0x10-0x13 保留/未知，跳过
  // AppName 位于 0x14 长度 32
  // IconName 位于 0x34 长度 32
  uint8_t type = buf[0x74];
  // SignatureTail 位于 0x75 长度 15
  uint32_t screen_w_mir = GET_U32(0x174);
  uint32_t screen_w = GET_U32(0x178);
  uint32_t screen_h = GET_U32(0x17C);
  uint32_t screen_h_mir = GET_U32(0x180);
  uint32_t sub_type = GET_U32(0x184);

  // 获取文件总大小（用于校验）
  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fclose(fp);

  // ---- 打印结果 ----
  printf("=== %s 文件头解析 ===\n", filename);
  printf("文件总大小       : %ld 字节\n", file_size);
  printf("头部长度         : 0x%X (%d) 字节\n", HEADER_SIZE, HEADER_SIZE);
  printf("PayloadSize      : %u 字节 (期望文件大小 = %u)\n", payload_size,
         HEADER_SIZE + payload_size);
  printf("\n");

  printf("AppletID         : 0x%08X (%u)\n", applet_id, applet_id);
  printf("Version          : 0x%08X (%u)\n", version, version);
  printf("Flags            : 0x%08X\n", flags);
  printf("Type             : 0x%02X", type);
  // 根据已知类型加注释
  if (type == 0x13)
    printf(" (标准)");
  else if (type == 0x9A)
    printf(" (扩展)");
  else
    printf(" (未知/子类型)");
  printf("\n");

  printf("AppName          : \"");
  print_safe(buf + 0x14, 32);
  printf("\"\n");

  printf("IconName         : \"");
  print_safe(buf + 0x34, 32);
  printf("\"\n");

  printf("SignatureTail    : ");
  for (int i = 0; i < 15; i++)
    printf("%02X", buf[0x75 + i]);
  printf("\n");

  printf("ScreenW (镜像)   : 0x%08X\n", screen_w_mir);
  printf("ScreenW          : 0x%08X (%u)\n", screen_w, screen_w);
  printf("ScreenH (镜像)   : 0x%08X\n", screen_h_mir);
  printf("ScreenH          : 0x%08X (%u)\n", screen_h, screen_h);
  printf("子类型/版本常量   : 0x%08X\n", sub_type);

  // 校验 payload 是否与文件大小一致
  if (file_size == HEADER_SIZE + payload_size) {
    printf("\n[✓] PayloadSize 与文件大小匹配\n");
  } else {
    printf("\n[!] PayloadSize 与文件大小不符 (实际 %ld)\n", file_size);
  }

  return 0;
}
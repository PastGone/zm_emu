#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "paser_info.h" /* AppletHeader：唯一定义在这里，本文件不再另抄一份 */

/* 头部固定大小 (392 字节) */
#define HEADER_SIZE 0x188

/* =========================================================================
 * 字段一律先按偏移从裸字节读出、再做 LE→主机字节序转换后存入结构体
 * ========================================================================= */

static uint32_t le32_to_host(const uint8_t *buf) {
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
         ((uint32_t)buf[3] << 24);
}

/* =========================================================================
 * 解析函数：从文件读取头部，填充结构体，并可选返回负载数据和大小
 * ========================================================================= */

bool parse_app_header(FILE *fp, AppletHeader *header) {

  uint8_t raw[HEADER_SIZE];

  if (fread(raw, 1, HEADER_SIZE, fp) != HEADER_SIZE) {
    fprintf(stderr, "错误：无法读取完整的头部（需要 %u 字节）\n", HEADER_SIZE);
    fclose(fp);
    return false;
  }

  /* 提取各字段并进行字节序转换 */
  header->AppletID = le32_to_host(raw + 0x000);
  header->Version = le32_to_host(raw + 0x004);
  header->Flags = le32_to_host(raw + 0x008);
  header->PayloadSize = le32_to_host(raw + 0x00C);
  header->Reserved_0x010 = le32_to_host(raw + 0x010);
  memcpy(header->AppName, raw + 0x014, 32);
  memcpy(header->IconName, raw + 0x034, 32);
  memcpy(header->Reserved_0x054, raw + 0x054, 32);
  memcpy(header->ProgramUID, raw + 0x074, 16);
  header->ActivationType = le32_to_host(raw + 0x084);
  memcpy(header->ActivationKey, raw + 0x088, 16);
  header->UnknownDataLength = le32_to_host(raw + 0x098);
  memcpy(header->UnknownData, raw + 0x09C, 216);
  header->MinScreenWidth = le32_to_host(raw + 0x174);
  header->MinScreenHeight = le32_to_host(raw + 0x178);
  header->MaxScreenWidth = le32_to_host(raw + 0x17C);
  header->MaxScreenHeight = le32_to_host(raw + 0x180);
  header->Unknown_0x184 = le32_to_host(raw + 0x184);

  /* 不再检查“镜像”相等性，因为已被新样本推翻
   * ,我从一个文件之中得到了它的真实尺寸或或者说是它的这个两个尺寸*/

  rewind(fp);
  return true;
}

/* =========================================================================
 * 打印函数：输出所有头部字段信息
 * ========================================================================= */
void print_header(const AppletHeader *h) {
  printf("========== .app 头部信息 ==========\n");
  printf("AppletID        : 0x%08X (%u)\n", h->AppletID, h->AppletID);
  printf("Version         : 0x%08X (%u)\n", h->Version, h->Version);
  printf("Flags           : 0x%08X\n", h->Flags);
  printf("PayloadSize     : 0x%08X (%u 字节)\n", h->PayloadSize,
         h->PayloadSize);
  printf("Reserved_0x010  : 0x%08X\n", h->Reserved_0x010);
  printf("AppName         : \"%.*s\"\n", 32, h->AppName);
  printf("IconName        : \"%.*s\"\n", 32, h->IconName);
  printf("ProgramUID      : ");
  for (int i = 0; i < 16; i++)
    printf("%02X ", h->ProgramUID[i]);
  printf("\n");
  printf("ActivationType  : 0x%08X (%u)\n", h->ActivationType,
         h->ActivationType);
  printf("ActivationKey   : ");
  for (int i = 0; i < 16; i++)
    printf("%02X ", h->ActivationKey[i]);
  printf("\n");
  printf("UnknownDataLen  : 0x%08X (%u)\n", h->UnknownDataLength,
         h->UnknownDataLength);
  printf("MinScreenW(0x174) : 0x%08X (%u)\n", h->MinScreenWidth,
         h->MinScreenWidth);
  printf("MinScreenH(0x178) : 0x%08X (%u)\n", h->MinScreenHeight,
         h->MinScreenHeight);
  printf("MaxScreenW(0x17C) : 0x%08X (%u px)\n", h->MaxScreenWidth,
         h->MaxScreenWidth);
  printf("MaxScreenH(0x180) : 0x%08X (%u px)\n", h->MaxScreenHeight,
         h->MaxScreenHeight);
  printf("Unknown_0x184   : 0x%08X (高16位:0x%04X, 低16位:0x%04X)\n",
         h->Unknown_0x184, (h->Unknown_0x184 >> 16) & 0xFFFF,
         h->Unknown_0x184 & 0xFFFF);
  printf("====================================\n");
}
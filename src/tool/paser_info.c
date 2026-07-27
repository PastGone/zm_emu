#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 头部固定大小 (392 字节) */
#define HEADER_SIZE 0x188

/* =========================================================================
 * 结构体定义：使用主机字节序，所有多字节整数已转换为本地字节序
 * ========================================================================= */
typedef struct {
  /* 起始 0x000，长度 4 —— AppletID（LE uint32） */
  uint32_t AppletID;

  /* 起始 0x004，长度 4 —— 版本号（LE uint32） */
  uint32_t Version;

  /* 起始 0x008，长度 4 —— 标志位（LE uint32），已观察 0x02/0x10/0x20 三类 */
  uint32_t Flags;

  /* 起始 0x00C，长度 4 —— 负载大小（LE uint32），0x188 + 此值 = 文件大小，9/9
   * 验证 */
  uint32_t PayloadSize;

  /* 起始 0x010，长度 4 —— 保留字段（推测），9/9 全零 */
  uint32_t Reserved_0x010;

  /* 起始 0x014，长度 32 —— 应用名称（UTF-8，0 终止） */
  char AppName[32];

  /* 起始 0x034，长度 32 —— 图标文件名（ASCII，通常为 "icon.zbmp"） */
  char IconName[32];

  /* 起始 0x054，长度 32 —— 保留字段（推测），9/9 全零 */
  uint8_t Reserved_0x054[32];

  /* 起始 0x074，长度 1 —— 类型（单字节，枚举特征，如 0x61 重复出现） */
  uint8_t Type;

  /* 起始 0x075，长度 15 —— 签名/校验数据（算法未还原，非裸 MD5） */
  uint8_t SignatureData[15];

  /* 起始 0x084，长度 4 —— 未知字段（待定），有重复值（0x190 / 0x7FFFFFFF） */
  uint32_t Unknown_0x084;

  /* 起始 0x088，长度 236 —— 保留/扩展数据区，部分样本有非零数据 */
  uint8_t Extended[236];

  /* 起始 0x174，长度 4 —— 未知字段（原误认为 ScreenW 镜像，现已推翻） */
  uint32_t Unknown_0x174;

  /* 起始 0x178，长度 4 —— 屏幕宽度（像素），值合理（128/176/220/240/320 等） */
  uint32_t ScreenW;

  /* 起始 0x17C，长度 4 —— 屏幕高度（像素），值合理（240/640/800/900 等） */
  uint32_t ScreenH;

  /* 起始 0x180，长度 4 —— 未知字段（原误认为 ScreenH 镜像，现已推翻） */
  uint32_t Unknown_0x180;

  /* 起始 0x184，长度 4 —— 未知字段，疑似高16位/低16位两个子字段 */
  uint32_t Unknown_0x184;

  /* 之后为负载（Payload），不包含在此结构体中 */
} AppHeader;

static uint32_t le32_to_host(const uint8_t *buf) {
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
         ((uint32_t)buf[3] << 24);
}

/* =========================================================================
 * 解析函数：从文件读取头部，填充结构体，并可选返回负载数据和大小
 * ========================================================================= */

bool parse_app_header(FILE *fp, AppHeader *header) {

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
  header->Type = raw[0x074];
  memcpy(header->SignatureData, raw + 0x075, 15);
  header->Unknown_0x084 = le32_to_host(raw + 0x084);
  memcpy(header->Extended, raw + 0x088, 236);
  header->Unknown_0x174 = le32_to_host(raw + 0x174);
  header->ScreenW = le32_to_host(raw + 0x178);
  header->ScreenH = le32_to_host(raw + 0x17C);
  header->Unknown_0x180 = le32_to_host(raw + 0x180);
  header->Unknown_0x184 = le32_to_host(raw + 0x184);

  /* 不再检查“镜像”相等性，因为已被新样本推翻 */

  fclose(fp);
  return true;
}

/* =========================================================================
 * 打印函数：输出所有头部字段信息
 * ========================================================================= */
void print_header(const AppHeader *h) {
  printf("========== .app 头部信息 ==========\n");
  printf("AppletID        : 0x%08X (%u)\n", h->AppletID, h->AppletID);
  printf("Version         : 0x%08X (%u)\n", h->Version, h->Version);
  printf("Flags           : 0x%08X\n", h->Flags);
  printf("PayloadSize     : 0x%08X (%u 字节)\n", h->PayloadSize,
         h->PayloadSize);
  printf("Reserved_0x010  : 0x%08X\n", h->Reserved_0x010);
  printf("AppName         : \"%.*s\"\n", 32, h->AppName);
  printf("IconName        : \"%.*s\"\n", 32, h->IconName);
  printf("Type            : 0x%02X\n", h->Type);
  printf("SignatureData   : ");
  for (int i = 0; i < 15; i++)
    printf("%02X ", h->SignatureData[i]);
  printf("\n");
  printf("Unknown_0x084   : 0x%08X\n", h->Unknown_0x084);
  printf("Unknown_0x174   : 0x%08X (%u)  [原误认为 ScreenW 镜像]\n",
         h->Unknown_0x174, h->Unknown_0x174);
  printf("ScreenW (0x178) : 0x%08X (%u px)\n", h->ScreenW, h->ScreenW);
  printf("ScreenH (0x17C) : 0x%08X (%u px)\n", h->ScreenH, h->ScreenH);
  printf("Unknown_0x180   : 0x%08X (%u)  [原误认为 ScreenH 镜像]\n",
         h->Unknown_0x180, h->Unknown_0x180);
  printf("Unknown_0x184   : 0x%08X (高16位:0x%04X, 低16位:0x%04X)\n",
         h->Unknown_0x184, (h->Unknown_0x184 >> 16) & 0xFFFF,
         h->Unknown_0x184 & 0xFFFF);
  printf("====================================\n");
}
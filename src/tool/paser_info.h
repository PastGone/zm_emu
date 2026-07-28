#ifndef __PARSE_INFO_H__
#define __PARSE_INFO_H__

// -------------------- 在这里放声明 --------------------
// 1. 函数声明
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  /* 起始 0x000，长度 4 —— AppletID（LE uint32） */
  uint32_t AppletID;

  /* 起始 0x004，长度 4 —— 版本号（LE uint32） */
  uint32_t Version;

  /* 起始 0x008，长度 4 —— 标志位（LE uint32），已观察 0x02/0x10/0x20 三类 */
  uint32_t Flags;

  /* 起始 0x00C，长度 4 —— 负载大小（LE uint32），0x188 + 此值 =
  文件大小，9/9
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

  /* 起始 0x084，长度 4 —— 未知字段（待定），有重复值（0x190 / 0x7FFFFFFF）
   */
  uint32_t Unknown_0x084;

  /* 起始 0x088，长度 236 —— 保留/扩展数据区，部分样本有非零数据 */
  uint8_t Extended[236];

  /* 起始 0x174，长度 4 —— 未知字段（原误认为 ScreenW 镜像，现已推翻） */
  uint32_t Unknown_0x174;

  /* 起始 0x178，长度 4 —— 屏幕宽度（像素），值合理（128/176/220/240/320 等）
   */
  uint32_t ScreenW;

  /* 起始 0x17C，长度 4 —— 屏幕高度（像素），值合理（240/640/800/900 等） */
  uint32_t ScreenH;

  /* 起始 0x180，长度 4 —— 未知字段（原误认为 ScreenH 镜像，现已推翻） */
  uint32_t Unknown_0x180;

  /* 起始 0x184，长度 4 —— 未知字段，疑似高16位/低16位两个子字段 */
  uint32_t Unknown_0x184;

  /* 之后为负载（Payload），不包含在此结构体中 */
} AppHeader;

bool parse_app_header(FILE *fp, AppHeader *header);
void print_header(const AppHeader *header);

#endif
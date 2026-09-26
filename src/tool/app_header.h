#ifndef APP_HEADER_H
#define APP_HEADER_H

// -------------------- 在这里放声明 --------------------
// 1. 函数声明
#include <stdbool.h>
#include <stddef.h> /* offsetof（结构体静态断言用） */
#include <stdint.h>
#include <stdio.h>

/* .app 头部共 0x188（392）字节；字段全是 1 字节对齐，故显式 pack，
 * 免得将来有人往里加字段时被编译器的对齐填充悄悄顶偏。 */
#pragma pack(push, 1)
typedef struct {
	/* 0x000 - 0x003：游戏 ID，小端 uint32 */
	uint32_t AppletID;

	/* 0x004 - 0x007：游戏版本号，小端 uint32 */
	uint32_t Version;

	/* 0x008 - 0x00B：标志位，可能用于区分软件/游戏类，具体未知 */
	uint32_t Flags;

	/* 0x00C - 0x00F：汇编程序大小，小端 uint32
	   文件大小 = 0x188 + PayloadSize */
	uint32_t PayloadSize;

	/* 0x010 - 0x013：留空 */
	uint32_t Reserved_0x010;

	/* 0x014 - 0x033：软件名称，UTF-8，通常 0 终止 */
	char AppName[32];

	/* 0x034 - 0x053：软件图标名称，通常为 "icon.zbmp" */
	char IconName[32];

	/* 0x054 - 0x073：留空 */
	uint8_t Reserved_0x054[32];

	/* 0x074 - 0x083：程序唯一 ID，16 字节，加密算法未知 */
	uint8_t ProgramUID[16];

	/* 0x084 - 0x087：软件激活类型，小端 uint32
	   例如 3 元、2 元、免费激活等 */
	uint32_t ActivationType;

	/* 0x088 - 0x097：软件激活成功后生成的密钥，16 字节，算法未知 */
	uint8_t ActivationKey[16];

	/* 0x098 - 0x09B：从 0x09C 开始的数据长度，小端 uint32 */
	uint32_t UnknownDataLength;

	/* 0x09C - 0x173：未知数据区，固定 216 字节
	   实际有效长度由 UnknownDataLength 指定；
	   游戏激活后可能生成，修改/删除无影响 */
	uint8_t UnknownData[216];

	/* 0x174 - 0x177：支持的最小屏幕宽度，小端 uint32 */
	uint32_t MinScreenWidth;

	/* 0x178 - 0x17B：支持的最小屏幕高度，小端 uint32 */
	uint32_t MinScreenHeight;

	/* 0x17C - 0x17F：支持的最大屏幕宽度，小端 uint32 */
	uint32_t MaxScreenWidth;

	/* 0x180 - 0x183：支持的最大屏幕高度，小端 uint32 */
	uint32_t MaxScreenHeight;

	/* 0x184 - 0x187：未知，可能和程序启动方式有关
	   原猜测疑似高 16 位/低 16 位两个子字段 */
	uint32_t Unknown_0x184;

	/* 之后为负载（Payload），不包含在此结构体中 */
} AppletHeader;
#pragma pack(pop)

/* 编译期钉死布局：改字段时这里会立刻报错，避免只在运行期才发现错位 */
_Static_assert(sizeof(AppletHeader) == 0x188, "AppletHeader 必须是 392 字节");

_Static_assert(offsetof(AppletHeader, AppName) == 0x014, "AppName 偏移错误");
_Static_assert(offsetof(AppletHeader, IconName) == 0x034, "IconName 偏移错误");
_Static_assert(offsetof(AppletHeader, Reserved_0x054) == 0x054, "Reserved_0x054 偏移错误");
_Static_assert(offsetof(AppletHeader, ProgramUID) == 0x074, "ProgramUID 偏移错误");
_Static_assert(offsetof(AppletHeader, ActivationType) == 0x084, "ActivationType 偏移错误");
_Static_assert(offsetof(AppletHeader, ActivationKey) == 0x088, "ActivationKey 偏移错误");
_Static_assert(offsetof(AppletHeader, UnknownDataLength) == 0x098, "UnknownDataLength 偏移错误");
_Static_assert(offsetof(AppletHeader, UnknownData) == 0x09C, "UnknownData 偏移错误");
_Static_assert(offsetof(AppletHeader, MinScreenWidth) == 0x174, "MinScreenWidth 偏移错误");
_Static_assert(offsetof(AppletHeader, MinScreenHeight) == 0x178, "MinScreenHeight 偏移错误");
_Static_assert(offsetof(AppletHeader, MaxScreenWidth) == 0x17C, "MaxScreenWidth 偏移错误");
_Static_assert(offsetof(AppletHeader, MaxScreenHeight) == 0x180, "MaxScreenHeight 偏移错误");
_Static_assert(offsetof(AppletHeader, Unknown_0x184) == 0x184, "Unknown_0x184 偏移错误");

bool parse_app_header(FILE *fp, AppletHeader *header);
void print_header(const AppletHeader *header);

#endif
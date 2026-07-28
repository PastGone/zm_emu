#ifndef __ZM_ADDRS_H__
#define __ZM_ADDRS_H__

// -------------------- 垫片 / 虚表对象地址表 --------------------
// 这些全局变量都在 main.c 中定义（由 SHIM_BASE 计算而来），
// 此处仅做 extern 声明，供 zmaee 各模块引用，避免在每个模块里重复硬编码。
#include <stdint.h>

/* root / runtime / gfx / fs / file / audio / ap 对象基址与虚表基址 */
extern uint32_t ROOT;
extern uint32_t RUNTIME;
extern uint32_t RT_VT;
extern uint32_t GFX;
extern uint32_t GFX_VT;
extern uint32_t FS;
extern uint32_t FS_VT;
extern uint32_t FILE1;
extern uint32_t FILE_VT;
extern uint32_t AUDIO;
extern uint32_t AUDIO_VT;
extern uint32_t AP;
extern uint32_t AP_VT;
extern uint32_t DUMMY_BUF;

/* sub_A98 注册回调使用的槽位 */
extern uint32_t SIZE_SLOT;
extern uint32_t API_SLOT;

#endif

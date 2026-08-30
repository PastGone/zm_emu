#ifndef U_MEM_EXT_H
#define U_MEM_EXT_H
/**
 * @file u_mem_ext.h
 * @brief mem* 的**非标准**扩展
 *
 * 这些函数在标准 C 里没有对应物，主要是 zmaee / 模拟器场景需要：
 *   u_memset16 / u_memset32  —— zmaee 图形模块按像素填充显存
 *   u_memrchr                —— GNU/POSIX 扩展，非 C 标准
 *   u_memcmp_host            —— 客户机缓冲区 vs 宿主缓冲区（模拟器便利函数）
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

/** memset16 / memset32：count 为「元素个数」，zmaee 图形模块常用 */
uint32_t u_memset16(uc_engine *uc, uint32_t dst, uint16_t v, uint32_t count);
uint32_t u_memset32(uc_engine *uc, uint32_t dst, uint32_t v, uint32_t count);

/** 反向查找字节 c（GNU 扩展语义），未找到返回 0 */
uint32_t u_memrchr(uc_engine *uc, uint32_t src, int c, uint32_t n);

/** 客户机缓冲区 a 与宿主缓冲区 b 比较（拿客户机路径和宿主常量比时最省事） */
int u_memcmp_host(uc_engine *uc, uint32_t a, const void *b, uint32_t n);

#endif /* U_MEM_EXT_H */

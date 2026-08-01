#ifndef DISASM_LOG_H
#define DISASM_LOG_H

#include <capstone/capstone.h>
#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

/**
 * 反汇编指定地址的指令并输出到日志（或 stderr）
 *
 * @param uc          Unicorn 引擎句柄
 * @param address     要反汇编的内存地址
 * @param code        预分配的字节缓冲区（至少 size 字节）
 * @param size        读取的最大字节数（建议 16）
 * @param cs_handle   已初始化的 Capstone 句柄
 *
 * @note 依赖全局变量 g_disasm（开关）和 log_trace 宏，需外部定义。
 */
void disassemble_and_log(uc_engine *uc, uint64_t address, size_t size);

#endif // DISASM_LOG_H
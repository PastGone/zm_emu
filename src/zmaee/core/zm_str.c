#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unicorn/unicorn.h>

// 读取 C 字符串
char *read_cstr(uc_engine *uc, uint32_t addr, char *buf, size_t maxlen) {
  if (addr == 0) {
    buf[0] = '\0';
    return buf;
  }
  size_t i = 0;
  while (i < maxlen - 1) {
    uint8_t c;
    uc_mem_read(uc, addr + i, &c, 1);
    if (c == 0)
      break;
    buf[i++] = c;
  }
  buf[i] = '\0';
  return buf;
}

/**
 * @brief 安全拷贝内存，取源长度和目标容量的较小值，防止越界
 * @param src     源地址 (对应 r0)
 * @param src_len 源缓冲区最大长度 (对应 r1)
 * @param dst     目标地址 (对应 r2)
 * @param dst_len 目标缓冲区容量 (对应 r3)
 * @return 实际拷贝的字节数 (n)
 */
uint32_t zm_strcpy(uc_engine *uc, uint32_t src, uint32_t src_len, uint32_t dst,
                   uint32_t dst_len) {
  // 取较小值作为拷贝长度（对应 r1 < r3 ? r1 : r3）
  size_t n = (src_len < dst_len) ? src_len : dst_len;
  uint8_t *data = malloc(n);

  if (data) {
    uc_mem_read(uc, src, data, n);
    uc_mem_write(uc, dst, data, n);
    free(data);
  }

  return n;
}

/**
 * @brief 格式化字符串并写入客户机目标地址（模拟简易 sprintf）
 * @param dest_addr      目标缓冲区客户机地址 (对应 r0)
 * @param fmt_addr       格式字符串客户机地址 (对应 r1)
 * @param args_addr      参数列表基址 (对应 r2)，第一个参数位于 args_addr + 4
 * @return uint32_t        写入目标缓冲区的字符串长度（不含结尾 '\0'）
 *
 * @note 当前实现仅支持 %u, %d, %s 三种格式，且仅处理第一个参数。
 *       若格式串不含上述格式，则直接复制原格式串。
 */
uint32_t zm_sprintf(uc_engine *uc, uint32_t dest_addr, uint32_t fmt_addr,
                    uint32_t args_addr) {
  // 1. 读取格式字符串到宿主机缓冲区
  char fmt[256];
  read_cstr(uc, fmt_addr, fmt, sizeof(fmt));

  // 2. 准备输出缓冲区
  char out[256];

  // 3. 根据格式类型，从参数列表（args_addr + 4）读取对应参数并格式化
  // 注意：由于格式串在客户机，参数紧跟在格式串参数之后，此处 args_addr
  // 指向参数数组， 因此第一个参数实际在 args_addr + 4（r2+4）
  // 因此第一个参数实际在 args_addr + 4（r2+4）
  uint32_t arg;
  if (strstr(fmt, "%u")) {

    uc_mem_read(uc, args_addr + 4, &arg, 4);
    snprintf(out, sizeof(out), fmt, arg);
  } else if (strstr(fmt, "%d")) {
    uc_mem_read(uc, args_addr + 4, &arg, 4); // 有符号转换
    snprintf(out, sizeof(out), fmt, arg);
  } else if (strstr(fmt, "%s")) {
    uint32_t str_addr = arg + 4; // 读取字符串指针
    char s[256];
    read_cstr(uc, str_addr, s, sizeof(s)); // 读取实际字符串内容
    snprintf(out, sizeof(out), fmt, s);
  } else {
    // 不包含特殊格式，直接拷贝原字符串
    strncpy(out, fmt, sizeof(out) - 1);
    out[sizeof(out) - 1] = '\0';
  }

  // 4. 计算长度并将结果写回客户机内存（包含 '\0' 结束符）
  size_t out_len = strlen(out);
  uc_mem_write(uc, dest_addr, out, out_len + 1); // 假设 uc 全局可用

  // 5. 返回写入的字符数（不含 '\0'）
  return out_len;
}

/**
 * @brief 将客户机字符串封装为一个自描述的结构体（指针+长度），并写回客户机
 * @param dest_struct  结构体基址（对应 r0）
 * @param src_str      源字符串地址（对应 r1）
 * @return uint32_t    返回 dest_struct（即 r0）
 */
uint32_t zm_str_ctor(uc_engine *uc, uint32_t dest_struct, uint32_t src_str) {

};

// strchr
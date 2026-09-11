#include "../../include/u_arg.h"

#include <string.h>

#include "../../include/u_mem.h"

/**
 * @file u_arg.c
 * @brief 【基建·非标准】AAPCS 变参游标
 *
 * 见 u_arg.h 顶部说明：宿主的 va_list 与客户机的 va_list 布局完全不同，
 * 这里用一个只认"4 字节槽序号"的游标统一三种取参来源。
 */

/* -------------------- 初始化 -------------------- */

void u_va_start_regs(u_va *va, uc_engine *uc, uint32_t n_named) {
  memset(va, 0, sizeof(*va));
  va->uc = uc;
  va->mode = U_VA_REGS;
  va->next = n_named;

  if (uc) {
    for (int i = 0; i < 4; i++) {
      uint64_t v = 0;
      if (uc_reg_read(uc, UC_ARM_REG_R0 + i, &v) == UC_ERR_OK)
        va->regs[i] = (uint32_t)v;
    }
    uint64_t sp = 0;
    if (uc_reg_read(uc, UC_ARM_REG_SP, &sp) == UC_ERR_OK)
      va->sp = (uint32_t)sp;
  }
}

void u_va_start_mem(u_va *va, uc_engine *uc, uint32_t addr,
                    uint32_t n_named) {
  memset(va, 0, sizeof(*va));
  va->uc = uc;
  va->mode = U_VA_MEM;
  va->mem_base = addr;
  va->next = n_named;
}

void u_va_start_mem8(u_va *va, uc_engine *uc, uint32_t addr,
                     uint32_t n_named) {
  memset(va, 0, sizeof(*va));
  va->uc = uc;
  va->mode = U_VA_MEM8;
  va->mem_base = addr;
  va->next = n_named;
}

void u_va_start_array(u_va *va, const uint32_t *args, uint32_t nargs) {
  memset(va, 0, sizeof(*va));
  va->uc = NULL;
  va->mode = U_VA_ARRAY;
  va->arr = args;
  va->arr_n = nargs;
  va->next = 0;
}

void u_va_start_ap(u_va *va, uc_engine *uc, uint32_t ap) {
  memset(va, 0, sizeof(*va));
  va->uc = uc;
  va->mode = U_VA_MEM;
  va->mem_base = ap;
  va->next = 0;
}

int u_va_start_va(u_va *va, uc_engine *uc, uint32_t va_list_addr) {
  if (!uc || va_list_addr == 0) {
    memset(va, 0, sizeof(*va));
    return -1;
  }
  uint32_t ap = u_rd32(uc, va_list_addr);
  if (ap == 0) {
    memset(va, 0, sizeof(*va));
    return -1;
  }
  u_va_start_ap(va, uc, ap);
  return 0;
}

void u_va_rewind(u_va *va, uint32_t n) {
  if (va->next >= n)
    va->next -= n;
  else
    va->next = 0;
}

void u_va_skip(u_va *va, uint32_t n) { va->next += n; }

uint32_t u_va_ptr(u_va *va) {
  switch (va->mode) {
  case U_VA_MEM:
    return va->mem_base + va->next * 4u;
  case U_VA_MEM8:
    return va->mem_base + 4u + va->next * 8u;
  case U_VA_REGS:
    if (va->next < 4)
      return 0; /* 还在寄存器里，没有内存地址 */
    return va->sp + (va->next - 4u) * 4u;
  default:
    return 0;
  }
}

/* -------------------- 取参 -------------------- */

static uint32_t va_slot(u_va *va, uint32_t n) {
  switch (va->mode) {
  case U_VA_MEM:
    return u_rd32(va->uc, va->mem_base + n * 4u);
  case U_VA_MEM8:
    /* 每槽 8 字节，值放槽首（槽 0 之前是变参个数） */
    return u_rd32(va->uc, va->mem_base + 4u + n * 8u);
  case U_VA_REGS:
    if (n < 4)
      return va->regs[n];
    return u_rd32(va->uc, va->sp + (n - 4u) * 4u);
  default: /* U_VA_ARRAY */
    if (n < va->arr_n && va->arr)
      return va->arr[n];
    return 0;
  }
}

/** AAPCS：64 位参数必须放在「偶序号寄存器对」或 8 字节对齐的栈地址 */
static void va_align64(u_va *va) {
  if (va->mode == U_VA_MEM8) {
    return; /* 每槽本身就是 8 字节对齐的独立单元 */
  }
  if (va->mode == U_VA_MEM) {
    if (((va->mem_base + va->next * 4u) & 7u) != 0u)
      va->next++;
  } else {
    if (va->next & 1u)
      va->next++;
  }
}

uint32_t u_va_u32(u_va *va) { return va_slot(va, va->next++); }

int32_t u_va_i32(u_va *va) { return (int32_t)u_va_u32(va); }

uint64_t u_va_u64(u_va *va) {
  if (va->mode == U_VA_MEM8) {
    /* applet 的溢出助手把 double 写成槽首起 8 字节，只占一个槽 */
    uint32_t addr = va->mem_base + 4u + va->next * 8u;
    uint32_t lo = u_rd32(va->uc, addr);
    uint32_t hi = u_rd32(va->uc, addr + 4u);
    va->next++;
    return ((uint64_t)hi << 32) | (uint64_t)lo;
  }
  va_align64(va);
  uint32_t lo = va_slot(va, va->next);
  uint32_t hi = va_slot(va, va->next + 1u);
  va->next += 2u;
  /* 客户机为小端 ARM：低 32 位在前 */
  return ((uint64_t)hi << 32) | (uint64_t)lo;
}

int64_t u_va_i64(u_va *va) { return (int64_t)u_va_u64(va); }

double u_va_f64(u_va *va) {
  uint64_t bits = u_va_u64(va);
  double d = 0.0;
  memcpy(&d, &bits, 8);
  return d;
}

float u_va_f32(u_va *va) {
  uint32_t bits = u_va_u32(va);
  float f = 0.0f;
  memcpy(&f, &bits, 4);
  return f;
}

void u_va_end(u_va *va) { (void)va; }

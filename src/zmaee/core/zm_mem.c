#include "../../log/log.h"
#include "stdint.h"

uint32_t host_malloc(uint32_t *heap_ptr, uint32_t size) {
  size = (size + 3) & ~3;
  uint32_t p = *heap_ptr;
  log_debug("从某某处分配了 %d 字节内存，当前堆指针为 %d", size, *heap_ptr);

  *heap_ptr += (size < 4 ? 4 : size);

  return p;
}
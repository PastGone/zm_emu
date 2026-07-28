#include "stdint.h"

uint32_t host_malloc(uint32_t *heap_ptr, uint32_t size) {
  size = (size + 3) & ~3;
  uint32_t p = *heap_ptr;
  *heap_ptr += (size < 4 ? 4 : size);
  return p;
}
#ifndef EMU_VT_TRAPS_H
#define EMU_VT_TRAPS_H

/* 各对象/接口虚表的 trap 宏，按接口拆分到独立子头，逐个 include 即可。
 * 任一子头都自带 include guard 且依赖 emu_mem_layout.h，可单独引用。 */
#include "traps/emu_bitmap_traps.h"
#include "traps/emu_display_traps.h"
#include "traps/emu_file_traps.h"
#include "traps/emu_image_traps.h"
#include "traps/emu_media_traps.h"
#include "traps/emu_root_traps.h"
#include "traps/emu_setting_traps.h"
#include "traps/emu_shell_traps.h"
#include "traps/emu_svc_traps.h"

#endif /* EMU_VT_TRAPS_H */

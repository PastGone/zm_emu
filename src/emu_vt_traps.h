#ifndef EMU_VT_TRAPS_H
#define EMU_VT_TRAPS_H

/* 各对象/接口虚表的 trap 宏，按接口拆分到独立子头，逐个 include 即可。
 * 任一子头都自带 include guard 且依赖 emu_mem_layout.h，可单独引用。 */
#include "emu_shell_traps.h"  /* IShell */
#include "emu_file_traps.h"   /* IFileMgr / IFile */
#include "emu_media_traps.h"  /* IMedia（音频） */
#include "emu_setting_traps.h" /* ISetting（配置读写） */
#include "emu_display_traps.h" /* IDisplay */
#include "emu_image_traps.h"  /* IImage / surface 门面 */
#include "emu_bitmap_traps.h" /* IBitmap */
#include "emu_svc_traps.h"    /* INetMgr / ITAPI / DLL / CBK / IUtil 等服务对象 */

#endif /* EMU_VT_TRAPS_H */

# 完善 00000405.app（号码归属）的 trap 与流程

## Summary（目标与范围）

把模拟环境从仅支持第一个 applet（00000102，23 个 trap）推进到**完整支持第二个 applet** `applet/00000405/00000405.app`（AppName="号码归属"，ScreenW=ScreenH=128）：
applet 能跑完 init、进入事件循环、响应触摸，且宿主侧所有被调用的外部接口都有可用的入口。

**范围（纳入）**：
1. 修 init 崩溃（`INIT_CTX` 上下文指针）
2. 补全 ROOT vtable 全部缺失槽位（已用 `.lst` 验证偏移，其中 0x60=memset、0x78=str_assign 给真实实现，其余 stub）
3. 新增 runtime 服务 0x1000004（SVC04）/ 0x1000009（SVC09）对象及其 vtable stub
4. `zm_fs` 升级为按名查表的多文件系统，支持 `fs.open("%s%08x.app")`、`config.b`、`zmsys006.dll` 等
5. `zm_sprintf` 支持多参数（`%s%08x.app`、`&dllversion=%d&dllname=%s`）
6. 新增 DLL 加载 trap（`RT_VT[+0x58]`）返回 stub DLL 对象 + 卸载 trap（`RT_VT[+0x5C]`），让 applet 调用 `sub_85248` 时不崩溃

**范围（不纳入，需单独工程）**：
- `zmsys006.dll` / `zmsys001.dll` 的**真实代码执行**（ARM DLL 解析+符号绑定+重定位，独立项目）
- `res/aee_phonenum` 等数据文件的真实业务读取（由 DLL 使用，applet 不直接 fs.open）
- 实际号码归属查询的端到端业务结果（依赖 DLL 协作）

DLL 以 stub 对象提供：applet 能调通 `loadDLL→init→entry` 链路不崩溃，但查询返回空。

## Current State Analysis（现状）

**已跑通（applet 1）**：23 个 trap（ROOT 8 槽 + RT 2 + GFX 6 + FS/file 4 + AUDIO/AP 3）+ `TR_init_callback`。init→绘制 25 按钮→音频播放 均正常。

**切换到 00000405 后的问题**（已通过反编译 `00000405.app.c` + 反汇编 `00000405.app.lst` 验证）：

1. **init 立即 MEM unmapped @0x100**：`TR_init_callback` 设 `r3=0`；applet init wrapper `sub_8433C(instance, a2, a3)` 在 `a2==0` 时执行 `v5 = a3[64]`（即解引用 `r3+0x100`）。真机此处传入非空"上下文结构体"指针。零填充 256B 缓冲即可（`*r3=0≠1`、`r3[64]=0≠4` → init 继续）。

2. **ROOT vtable 缺 23 个槽**：applet 通过 `[BLOB_BASE+0x180]`（`off_80180`）取 ROOT 指针，`(*ROOT+offset)` 调用大量导入。当前 `zm_emu_build_vtables` 只填 8 槽。每个 `sub_84xxx` thunk 的 `.lst` 已验证偏移（见下表）。

3. **新 runtime 服务**：init 主体 `sub_841D4` 通过 `rt.queryInterface`（`RT_VT[+8]`）请求 `0x1000009`→SVC09（存 v2+0x210/528）、`0x1000004`→SVC04（存 v2+0x204/516）、`0x1000003`→FS（存 v2+0x208/520）。当前 `zm_rt_queryInterface` 只识别 0x1000003/5/B/C。

4. **多文件 fs.open**：`sub_84FD8`/`sub_85128` 用 `sprintf("%s%08x.app", path, id)` 拼文件名后 `fs.open`，再 `file.read(buf, 392)` 读 applet 自身头（id=0x405=1029）。当前 `zm_fs.c` 只支持单个 `.zmr`（且 00000405 目录无 .zmr）。

5. **DLL 加载**：`sub_85248` 调 `(*RUNTIME+88)(RUNTIME, "zmsys001.dll", 28, &out)` = `RT_VT[+0x58]`（loadDLL），返回 DLL 对象；随后调其 `vt[+8]`、`vt[+12]`、`vt[+16]`。`sub_85340` 调 `RT_VT[+0x5C]`（unloadDLL）。**注：init 不调 DLL 加载，仅事件流调**，但为完整性一并补上 stub。

### 已验证的 ROOT vtable 槽位表（`.lst` 实测）

| Thunk | ROOT offset | 参数 | 处理方式 | 备注 |
|-------|-------------|------|---------|------|
| sub_843E8 | 0x00 | 0 | ✓ queryRuntime | 已实现 |
| sub_84410 | 0x08 | 1(size∈R0) | ✓ malloc | 已实现；sub_841D4 用 `MOV R0,#0x214` 调 |
| sub_84424 | 0x0C | 1(ptr) | ✓ free | 已实现 |
| sub_84460 | 0x18 | 0 | stub | |
| sub_84474 | 0x1C | 6(4寄存器+栈) | stub | 栈传参 thunk |
| sub_844C0 | 0x24 | 0 | stub | |
| sub_8470C | 0x50 | 0 | stub（返 0 表不支持） | sub_84FD8 检查返回值 |
| sub_84748 | 0x5C | 0 | stub | |
| sub_8475C | 0x60 | 3(dst,val,len) | **memset 真实实现** | 0x841EC 验证：`MOV R2,#0x214;MOV R1,#0;BL sub_8475C` |
| sub_84798 | 0x6C | vargs | ✓ sprintf（扩多参数） | |
| sub_84854 | 0x74 | 0 | stub | |
| sub_84868 | 0x78 | 2(str_obj,cstr) | **str_assign 真实实现** | 0x84274 验证：`ADR R1,"app_list";BL sub_84868` |
| sub_8487C | 0x7C | 0 | stub | |
| sub_84890 | 0x80 | 0 | stub | |
| sub_848A4 | 0x84 | 0 | stub | |
| sub_848B8 | 0x88 | 2 | ✓ str_ctor | 已实现 |
| sub_848CC | 0x8C | 0 | stub | |
| sub_848E0 | 0x90 | 0 | stub | |
| sub_84944 | 0xA8 | 2 | ✓ str_find | 已实现 |
| sub_8496C | 0xB0 | 0 | stub | |
| sub_849BC | 0xC0 | 0 | stub | |
| sub_849E4 | 0xC8 | 0 | stub | |
| sub_84A0C | 0xD0 | 0 | stub | |
| sub_84A34 | 0xD8 | 0 | stub（返时间戳） | 疑似 get_tick，返 `SDL_GetTicks()` |
| sub_84BD8 | 0x12C | 4 | stub | |
| sub_84C04 | 0x130 | 4 | stub | |
| sub_84C6C | 0x140 | 4 | stub | |
| sub_84E04 | 0x154 | 0 | **返 shim 对象** | vt[+8] 被 applet 覆写为 sub_82FF8（定时器回调） |
| sub_84E7C | 0x16C | 4+栈 | stub | |

### 新增服务对象 vtable 槽（已验证调用点）

| 对象 | vtable 槽 | 来源 | 处理 |
|------|----------|------|------|
| SVC04_VT | +0x04 | release | stub（返 0） |
| SVC04_VT | +0x1C | 行 2419 `(a1+516)+28` 调用 | stub |
| SVC09_VT | +0x04 | release | stub |
| SVC09_VT | +0x2C | 行 2434 `(a1+528)+44` 调用 | stub |
| SVC09_VT | +0x40 | 行 2433 `(a1+528)+64` 调用 | stub |
| FS_VT | +0x04 | release | stub |
| FS_VT | +0x14 | 0x84288 chdir(str_obj) | stub（返 0，可选记录 cwd） |
| RT_VT | +0x58 | loadDLL(name,len,out) | stub：返回 DLL 对象 |
| RT_VT | +0x5C | unloadDLL(handle) | stub |
| DLL 对象 VT | +0x08 / +0x0C / +0x10 | sub_85248 调用 | stub（返 0） |
| CBK 对象 VT | +0x08 | sub_84E04 返回，被覆写为 sub_82FF8 | stub（初始值，会被覆写） |

## Proposed Changes（按文件）

### 1. [src/emu.h](file:///home/apollo/文档/古时游戏/zm_emu/src/emu.h) — 新增声明
- 新增 shim 对象地址 `extern`：`SVC04`、`SVC04_VT`、`SVC09`、`SVC09_VT`、`INIT_CTX`、`CBK_OBJ`、`CBK_OBJ_VT`、`DLL_OBJ`、`DLL_OBJ_VT`。
- 新增 trap 地址 `extern`（索引 23 起，沿用 `TRAP(idx)`）：
  - ROOT stub：`TR_root_x18 / x1C / x24 / x50 / x5C / x74 / x7C / x80 / x84 / x8C / x90 / xB0 / xC0 / xC8 / xD0 / xD8 / x12C / x130 / x140 / x16C`
  - ROOT 真实：`TR_root_memset`（0x60）、`TR_root_str_assign`（0x78）、`TR_root_create_cbk`（0x154）
  - 服务：`TR_svc_release`、`TR_svc04_x1C`、`TR_svc09_x2C`、`TR_svc09_x40`
  - FS：`TR_fs_release`、`TR_fs_chdir`
  - RT：`TR_rt_loadDLL`、`TR_rt_unloadDLL`
  - DLL：`TR_dll_init`（+8）、`TR_dll_config`（+0xC）、`TR_dll_entry`（+0x10）

### 2. [src/emu.c](file:///home/apollo/文档/古时游戏/zm_emu/src/emu.c) — 定义与 vtable 构建
- 定义上述对象地址（`SHIM_BASE + 0x800` 起分区，避免与 `DUMMY_BUF@0x750` 冲突）：
  - `INIT_CTX = SHIM_BASE + 0x800`（256B 零填充）
  - `SVC04 = SHIM_BASE + 0x900`、`SVC04_VT = SHIM_BASE + 0x910`
  - `SVC09 = SHIM_BASE + 0x940`、`SVC09_VT = SHIM_BASE + 0x950`
  - `CBK_OBJ = SHIM_BASE + 0x980`、`CBK_OBJ_VT = SHIM_BASE + 0x990`（可写）
  - `DLL_OBJ = SHIM_BASE + 0x9C0`、`DLL_OBJ_VT = SHIM_BASE + 0x9D0`
- 定义所有新 `TR_* = TRAP(idx)`。
- `zm_emu_build_vtables` 中：
  - 把 23 个 ROOT stub/真实 trap 写入对应 `ROOT + offset`。
  - `SVC04[0]=SVC04_VT`；`SVC04_VT[+4]=TR_svc_release`、`[+0x1C]=TR_svc04_x1C`。
  - `SVC09[0]=SVC09_VT`；`SVC09_VT[+4]=TR_svc_release`、`[+0x2C]=TR_svc09_x2C`、`[+0x40]=TR_svc09_x40`。
  - `FS_VT[+0x04]=TR_fs_release`、`FS_VT[+0x14]=TR_fs_chdir`。
  - `RT_VT[+0x58]=TR_rt_loadDLL`、`RT_VT[+0x5C]=TR_rt_unloadDLL`。
  - `CBK_OBJ[0]=CBK_OBJ_VT`；`CBK_OBJ_VT[+8]=TR_cbk_default`（初始 stub，会被覆写）。
  - `DLL_OBJ[0]=DLL_OBJ_VT`；`[+8]/[+0xC]/[+0x10]=TR_dll_*`。
  - 零填充 `INIT_CTX`、`CBK_OBJ_VT` 等可写区域（确保 `uc_mem_write` 前 region 已 map，SHIM 区已 `UC_PROT_ALL` 可写）。

### 3. [src/trap.c](file:///home/apollo/文档/古时游戏/zm_emu/src/trap.c) — 调度与 init 修复
- **`TR_init_callback`**：把 `r3 = INIT_CTX`（替换 `zero`）。其它寄存器不变（r0=INSTANCE, r1=0, r2=0）。
- **重构 dispatch**：建议把 `handle_trap` 的 if-else 链改为 `switch ((trap_address - TRAMP_BASE) / 4)`（trap 索引），`case` 调对应 `zm_*` 函数，`default` 走现有"非法的外部调用"日志。保留现有分支语义。
- 新增分支调用 `zm_root_*`、`zm_rt_loadDLL` 等（见下）。
- `TR_init_callback` 与 `TR_ap_play` 等既有分支保留。

### 4. [src/event.c](file:///home/apollo/文档/古时游戏/zm_emu/src/event.c) — init 事件路径
- `dispatch_applet_event`：当 `evt==0`（init）时设 `r3 = INIT_CTX`，保证再次进入 init 也安全。其它事件不变（r3=y）。

### 5. 新增 [src/zmaee/core/zm_root.c](file:///home/apollo/文档/古时游戏/zm_emu/src/zmaee/core/zm_root.c) / [zm_root.h](file:///home/apollo/文档/古时游戏/zm_emu/src/zmaee/core/zm_root.h)
- `zm_root_memset(uc, dst, val, len)`：读 `len` 不做越界检查（信任 applet），用 `uc_mem_write` 写 `val` 填充（`calloc`+`uc_mem_write`）。
- `zm_root_str_assign(uc, str_obj, cstr_ptr)`：读 cstr（`read_cstr`），写入 str_obj 内联缓冲（`str_obj+12`），更新 `+4` 长度、`+8` 容量，`+0` 指针指向 `str_obj+12`（与 `zm_str_ctor` 布局一致）。
- `zm_root_create_cbk(uc)`：返回 `CBK_OBJ`（applet 随后会覆写 `CBK_OBJ_VT[+8]` 为 `sub_82FF8`，VT 可写即可）。
- `zm_root_stub(uc, offset, r0, r1, r2, r3)`：统一 `log_info("stub root[0xNN] r0=%u r1=%u r2=%u r3=%u", ...)` 返回 0。`0xD8` 特例返 `SDL_GetTicks()`（需 `#include <SDL2/SDL.h>`）。
- `0x50`（sub_8470C）特例：sub_84FD8 检查其返回值 `!=0` 才继续，返 0 表"不支持"会让 sub_84FD8 提前 return——但 sub_84FD8 不在 init 路径，返 0 可接受。

### 6. [src/zmaee/runtime/zm_runtime.c](file:///home/apollo/文档/古时游戏/zm_emu/src/zmaee/runtime/zm_runtime.c) — 新服务 + DLL
- `zm_rt_queryInterface` 增加：`case 0x1000004 → SVC04`、`case 0x1000009 → SVC09`。
- 新增 `zm_rt_loadDLL(uc, name_ptr, name_len, out_ptr)`：读 dll 名（log），写 `*out_ptr = DLL_OBJ`，返回 `DLL_OBJ`（非 0 表成功）。
- 新增 `zm_rt_unloadDLL(uc, handle)`：log + 返 0。
- `zm_rt_loadDLL` 的服务对象 vtable 方法（`TR_dll_*`）实现为 stub：log + 返 0。

### 7. [src/zmaee/fs/zm_fs.c](file:///home/apollo/文档/古时游戏/zm_emu/src/zmaee/fs/zm_fs.c) / [zm_fs.h](file:///home/apollo/文档/古时游戏/zm_emu/src/zmaee/fs/zm_fs.h) — 多文件系统
- 保留 `.zmr` 单游标路径（applet 1 兼容，`zm_fs_load_zmr`/`zm_fs_get_resource` 不动）。
- 新增多文件层：
  - `typedef struct { char name[64]; const uint8_t *data; size_t size; uint32_t pos; bool in_use; } file_slot_t;` 静态 `s_slots[8]`。
  - `bool zm_fs_register_file(const char *name, const void *data, size_t size)`：按 basename 登记到内部表（data 由调用方持有，如 mmap 或读入缓冲）。
  - `zm_fs_register_default(app_dir)`：注册 `00000405.app`（自身，读入内存）、`config.b`、`zmsys006.dll`、`1_32icon.zbmp`（按 `app_list/` 与 `res/` 路径加载）。
- `zm_fs_open(filename_ptr)` 改造：
  - 读 r1 为 C 字符串（兼容 zmaee 字符串对象：若首 4B 像指针则取 `[r1]`；尝试两种解析）。
  - 提取 basename（最后一个 `\` 或 `/` 之后）。
  - 查表命中 → 分配空 slot，`pos=0`，返回 `FILE1 + idx*0x10`（句柄可区分多文件）；未命中 → 返回 0（applet 会检查）。
- `zm_file_read/close/seek` 增加 `file_id`（r0）参数，按句柄 `((file_id-FILE1)/0x10)` 索引到 slot。**保留旧 `.zmr` 单游标**：当 `file_id == FILE1` 且 `.zmr` 已载入且 slot 表无对应项时，回退到旧路径（applet 1 兼容）。
- 在 `main.c` 启动时调 `zm_fs_register_default(applet_dir)`。

### 8. [src/zmaee/core/zm_str.c](file:///home/apollo/文档/古时游戏/zm_emu/src/zmaee/core/zm_str.c) — 多参数 sprintf
- `zm_sprintf(uc, dest, fmt_addr, args_addr)` 改为遍历 fmt：
  - 遇 `%` 读下一字符分派 `d/i/u/x/X/o/c/s/p/f`。
  - 参数从 `args_addr + 4` 起每 4B 取一个（`%f` 取 8B，双字对齐）。
  - 用宿主 `vsnprintf` 或手工拼接写入 `out[256]`，`uc_mem_write` 回 dest，返回长度（不含 `\0`）。
  - 边界 256B（足够 `"%s%08x.app"`、`"&dllversion=%d&dllname=%s"`）。
- 保留 `read_cstr`、`zm_strcpy`、`zm_str_ctor`、`zm_spec_lookup`、`zm_str_find`。

### 9. [src/main.c](file:///home/apollo/文档/古时游戏/zm_emu/src/main.c)
- 文件名已是 `00000405.app`（第 45-46 行），不动。
- `zm_emu_load_zmr_if_exists` 后增加：从 `filename` 推导 applet 目录，调 `zm_fs_register_default(dir)` 登记多文件。
- 既有调试开关（`ZM_AUDIO_TEST`/`ZM_AUTO_CLICK`/`ZM_DUMP_BUTTONS` 等）保留；`ZM_DUMP_BUTTONS` 的按钮布局假设（25 按钮 @ INSTANCE+124）对 00000405 不适用，改为仅在 `g_instance` 非零时安全读取（已如此）。

## Assumptions & Decisions

1. **DLL 不真实执行**：`zmsys006.dll` 头部结构与 `.app` 相同（已 `od` 确认：AppletID=0x1776、PayloadSize=0xE0A8=57904-0x188），但真实执行需独立工程。本计划用 stub DLL 对象让链路不崩溃，查询返回空。
2. **先 stub 后细化**：除 memset/str_assign/create_cbk 给真实实现（init 必需），其余 ROOT 槽先 stub（log+返 0）。迭代依据 `/tmp/r405.log` 中 `stub root[0xNN]` 命中频次逐个替换。
3. **trap 索引分配**：ROOT stub/真实 23 个用 23..45；服务/FS/RT/DLL/CBK 用 46..57。均远小于 TRAMP 容量。
4. **dispatch 重构为 switch**：35+ 分支用 if-else 难维护，改为按 trap 索引 switch。`TR_init_callback`（索引 100）单独保留。
5. **`.zmr` 兼容**：00000405 无 `.zmr`，`zm_emu_load_zmr_if_exists` 会跳过（已 log）。多文件 fs 不影响 applet 1 的 `.zmr` 单游标路径。
6. **INIT_CTX 选址**：`SHIM_BASE + 0x800`，256B 零填充。与 SVC04/09/CBK/DLL 对象区不重叠（0x900 起）。
7. **CBK_OBJ_VT 必须可写**：applet 会 `STR sub_82FF8, [VT+8]`。SHIM 区 `UC_PROT_ALL` 已可写，无需额外映射。
8. **fs.open basename 匹配**：applet 传入可能含路径前缀（`app_list\config.b`），按最后一个分隔符取 basename 比对。

## Verification（验证步骤）

```bash
xmake b zm_emu

# 1. 头跑：init 不再 MEM unmapped，trap 全命中已知分支
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ZM_DISASM=0 ZM_GFX_HOLD_MS=0 \
  ./build/linux/x86_64/release/zm_emu < /dev/null 2>&1 | tee /tmp/r405.log

# 期望：
grep -E "非法|MEM unmapped" /tmp/r405.log          # 应为空
grep -cE "stub root\[" /tmp/r405.log               # >0，记录高频项
grep -E "instance=|handler=|虚表构建完成|fs.open" /tmp/r405.log
grep -E "queryInterface.*0x1000004|0x1000009" /tmp/r405.log   # 服务请求可见
```

迭代：
- 看哪些 `stub root[0xNN]` 频繁出现，按需替换为真实实现（如 `0x5C`/`0x60` 已是 memset，`0xD8`→get_tick）。
- 用 `ZM_AUTO_CLICK="64,64"` 注入一次点击，观察事件流是否触达 DLL 加载（`RT_VT[+0x58]`）与 fs.open。

## 实施顺序（建议 todo）

1. Step A（解锁 init）：emu.h/emu.c 加 `INIT_CTX`；trap.c 改 `r3=INIT_CTX`；event.c `evt==0` 设 r3。→ 验证 init 不崩。
2. Step B（ROOT 槽）：emu.c 写 23 槽 + 新建 zm_root.c（memset/str_assign/create_cbk 真实，余 stub）；trap.c dispatch 补分支。→ 验证 init 跑完、进事件循环。
3. Step C（服务）：emu.c 建 SVC04/09 对象与 VT；zm_runtime.c 加 queryInterface 分支与 stub 方法。→ 验证服务请求返回非 0。
4. Step D（多文件 fs）：zm_fs.c 多 slot + register_default；main.c 调用。→ 验证 `fs.open("...00000405.app")` + `file.read(392)` 成功。
5. Step E（sprintf 多参数）：zm_str.c 改造。→ 验证 `%s%08x.app` 拼接正确。
6. Step F（DLL stub）：emu.c 建 DLL_OBJ/VT；zm_runtime.c 加 loadDLL/unloadDLL + DLL 方法 stub。→ 验证 `sub_85248` 调用不崩。

# Windows 环境搭建与编译运行指南

> 适用对象：拿到本项目**源码**后，想在 Windows 10/11（x64）上把它编译跑起来的同学。
> 全程只需要装两个东西：**Visual Studio Build Tools**（编译器）+ **xmake**（构建工具），其余依赖由 xmake 自动联网下载编译。

---

## 一、为什么 Windows 上要特殊处理

- 本项目源码用了 **C23** 特性（`constexpr` 对象、`enum X : uint32_t` 带底层类型的枚举）。
- MSVC 自带的 `cl.exe` 最高只支持到 C17，编不过，所以 Windows 上改用 **clang-cl**（VS 自带的 Clang，ABI 与 MSVC 完全一致）。
- `clang-cl` 依赖 MSVC 的头文件和库 + Windows SDK，因此**必须**先装 VS Build Tools。
- 这些已经全部写进 `xmake.lua`，你不用改任何配置，装好环境后直接编译即可。

---

## 二、一次性环境准备（只需装一次）

### 2.1 安装 Visual Studio Build Tools（提供 clang-cl + Windows SDK）

1. 打开下载页，下载 **Build Tools 安装器**：
   https://visualstudio.microsoft.com/zh-hans/visual-cpp-build-tools/
2. 运行安装器，在工作负载里勾选 **「使用 C++ 的桌面开发」**（英文：Desktop development with C++）。
3. 在右侧 **「单个组件 / 可选」** 列表里，确认勾选以下三项：
   - **MSVC v143（或更新版本）x64/x86 生成工具** —— 提供 `cl.exe` 与 C/C++ 头文件、库
   - **Windows 11（或 Windows 10）SDK** —— 提供系统头文件
   - **适用于 Windows 的 C++ Clang 工具**（C++ Clang tools for Windows）—— **关键项**，缺了会报 `找不到 clang-cl`
4. 点安装（需要 2~5 GB 空间，耐心等待）。

> 也可以命令行静默安装（把 `vs_BuildTools.exe` 换成你下载到的文件名）：
> ```
> vs_BuildTools.exe --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --includeRecommended --passive --norestart
> ```
> （若组件 ID 因 VS 版本不同略有出入，以图形界面勾选为准。）

### 2.2 安装 xmake（构建工具）

任选其一即可：

| 方式 | 命令 |
|------|------|
| **winget（推荐）** | `winget install xmake` （或 `winget install xmake-io.xmake`） |
| scoop | `scoop install xmake` |
| 官方安装脚本 | `irm https://xmake.io/psget.text \| iex` |
| 官方安装包 | 到 https://github.com/xmake-io/xmake/releases 下载 `.exe` 双击安装 |

安装完成后，**新开一个终端**，验证：

```powershell
xmake --version
```

能打印出 `xmake v3.x.x ...` 即成功。

### 2.3 （推荐）安装 Git

xmake 下载 `dirent` 这个依赖包时会用 git 克隆，装了 Git 更稳：
https://git-scm.com/download/win

---

## 三、拿到代码并编译

1. 解压别人发给你的代码压缩包，得到一个项目目录（能看到 `xmake.lua` 的那一层）。
2. 在该目录打开 PowerShell（在文件夹地址栏输入 `powershell` 回车即可，或用 `cd` 进去）。
3. 首次配置并编译（**需要联网，首次较慢**，会自动下载编译 6 个依赖库）：

```powershell
xmake f -p windows -a x64 -m release -y
xmake -y
```

首次大概几分钟；成功后会看到 `[100%]: build ok`，产物在：

```
build\windows\x64\release\zm_emu.exe
```

---

## 四、运行

```powershell
xmake run
```

或直接运行：

```powershell
.\build\windows\x64\release\zm_emu.exe
```

程序默认加载 `applet/` 目录下的默认 applet，命令行参数见：

```powershell
xmake run -- --help
```

---

## 五、常见问题（FAQ）

### Q1：中文日志是乱码怎么办？
本项目启动时已自动把控制台代码页切到 UTF-8，一般不会乱。若仍乱码：
- 运行前先执行 `chcp 65001`；
- 或改用 **Windows Terminal**（微软商店可下，默认 UTF-8，显示效果最好）。

### Q2：下载 `dirent` 包失败（报 SSL/TLS 错误）
报错类似 `SSL routines::unexpected eof`，是 GitHub 网络抖动，处理办法：
- 多试几次 `xmake -y`；
- 或手动下载：https://codeload.github.com/tronkko/dirent/tar.gz/refs/tags/1.26 ，把下载文件改名为 `dirent-1.26.tar.gz`，放到 xmake 报错日志里提示的缓存目录（形如 `%LOCALAPPDATA%\.xmake\cache\packages\...\d\dirent\1.26\`）再重跑。

### Q3：报「找不到 cl」或「找不到 clang-cl」？
说明 VS Build Tools 没装全。重点检查 2.1 节的三项组件，尤其是 **「适用于 Windows 的 C++ Clang 工具」**。装完记得**新开终端**再编译。

### Q4：提示 `target(zm_emu) maybe is not compatible with license(GPL-2.0) of package(unicorn)`？
这是 xmake 的许可证善意提醒（unicorn 是 GPL-2.0），**不影响编译运行**，一般可忽略；发布二进制时注意合规即可。

### Q5：想省事，能不能直接要编译好的 exe？
可以。作者把 `build\windows\x64\release\zm_emu.exe` 连同项目里的 `applet\`、`res\` 等资源目录一起按原目录结构发给你，exe 是静态链接、基本单文件，可直接运行；但注意 exe 运行时是按相对路径找 `applet/` 资源的，别打散目录结构。

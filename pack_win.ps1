#Requires -Version 5.1
<#
.SYNOPSIS
    zm_emu Windows 便携包打包脚本
    （把编译产物 + applet 资源 + 自带字体 + VC 运行库打成一个 zip，解压后
      双击 zm_emu.exe 就能跑，适合直接发给朋友）

.DESCRIPTION
    包内结构（必须保持 exe 与资源同目录，程序是按**相对路径**找资源的）:
        zm_emu_win_x64/
            zm_emu.exe                      主程序
            vcruntime140.dll                VC++ 运行库（可再分发，免装）
            applet/                         全部 applet 资源
            src/zmaee/unifont_t-18.0.01.pcf 自带像素字体（含全部汉字字形）
            使用说明.txt

    不打包 res/ —— 那是素材/参考包，运行时用不到（applet 请求的 "res\xxx"
    是各自 applet 目录**内部**的相对路径）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\pack_win.ps1
    # 构建 + 打包，产物: dist\zm_emu_win_x64.zip

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\pack_win.ps1 -NoBuild -NoZip
    # 跳过构建、只生成目录（不压缩），便于自己先试跑

.NOTES
    若提示"禁止运行脚本"，用上面示例里的 -ExecutionPolicy Bypass 方式启动。
#>

[CmdletBinding()]
param(
    # 输出目录（相对项目根）
    [string]$OutDir = 'dist',
    # 包名（同时作为解压后的顶层文件夹名）
    [string]$Name = 'zm_emu_win_x64',
    # 跳过 xmake 构建（复用已有产物）
    [switch]$NoBuild,
    # 只生成目录，不压缩成 zip
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'

# 中文输出，PowerShell 5.1 下要设成 UTF-8
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$OutputEncoding = [System.Text.Encoding]::UTF8

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $ScriptDir

# ============================================================
# 1. 构建
# ============================================================
if (-not $NoBuild) {
    $xmakeCmd = Get-Command xmake -ErrorAction SilentlyContinue
    if ($xmakeCmd) {
        $xmakeExe = $xmakeCmd.Source
    } else {
        $cand = Join-Path $env:USERPROFILE 'xmake\xmake.exe'
        if (Test-Path -LiteralPath $cand) { $xmakeExe = $cand }
        else { Write-Error "找不到 xmake。请先安装 xmake，或加 -NoBuild 复用已有构建产物。"; exit 1 }
    }
    Write-Host "构建中 (xmake) ..."
    & $xmakeExe build -y
    if ($LASTEXITCODE -ne 0) { Write-Error "构建失败（exit=$LASTEXITCODE）"; exit 1 }
}

# ============================================================
# 2. 校验源文件
# ============================================================
$EXE = Join-Path $ScriptDir 'build\windows\x64\release\zm_emu.exe'
if (-not (Test-Path -LiteralPath $EXE)) {
    $found = Get-ChildItem -Path (Join-Path $ScriptDir 'build\windows') `
        -Filter 'zm_emu.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($found) { $EXE = $found.FullName }
}
if (-not (Test-Path -LiteralPath $EXE)) {
    Write-Error "找不到 zm_emu.exe（路径: $EXE）。先执行 xmake build。"
    exit 1
}

$APPLET = Join-Path $ScriptDir 'applet'
if (-not (Test-Path -LiteralPath $APPLET)) { Write-Error "找不到 applet 目录: $APPLET"; exit 1 }

# 自带像素字体：程序按 "src/zmaee/unifont_t-18.0.01.pcf"（相对项目根/工作目录）
# 查找，包里必须保持同样的相对路径。
$FONT_REL = 'src\zmaee\unifont_t-18.0.01.pcf'
$FONT = Join-Path $ScriptDir $FONT_REL
if (-not (Test-Path -LiteralPath $FONT)) { Write-Error "找不到自带字体: $FONT"; exit 1 }

# ============================================================
# 3. 准备输出目录
# ============================================================
$pkgDir = Join-Path $ScriptDir (Join-Path $OutDir $Name)
if (Test-Path -LiteralPath $pkgDir) {
    Write-Host "清理旧目录: $pkgDir"
    Remove-Item -LiteralPath $pkgDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pkgDir 'src\zmaee') | Out-Null

Write-Host "复制主程序 ..."
Copy-Item -LiteralPath $EXE -Destination (Join-Path $pkgDir 'zm_emu.exe') -Force

Write-Host "复制 applet 资源（约 94MB，稍等）..."
# robocopy 比 Copy-Item 快很多；退出码 0-7 都算成功，>=8 才是错误
$null = robocopy $APPLET (Join-Path $pkgDir 'applet') /E /NFL /NDL /NJH /NJS /NP
if ($LASTEXITCODE -ge 8) { Write-Error "复制 applet 失败（robocopy exit=$LASTEXITCODE）"; exit 1 }

Write-Host "复制自带字体 ..."
Copy-Item -LiteralPath $FONT -Destination (Join-Path $pkgDir $FONT_REL) -Force

# ============================================================
# 4. 附带 VC++ 运行库（exe 依赖 VCRUNTIME140.dll）
#    优先用 VS 的 Redist 目录（官方允许再分发），兜底系统目录。
# ============================================================
$dllCopied = $false
# 注意：含通配符的 -Path 再叠加 -Filter 组合**不可靠**（实测匹配不到，会静默
# 落到 System32 兜底），所以这里把文件名直接拼进通配符路径里。
$redistGlob = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\*\BuildTools\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT\vcruntime140.dll'
$crt = Get-ChildItem -Path $redistGlob -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $crt) {
    $sys = Join-Path $env:WINDIR 'System32\vcruntime140.dll'
    if (Test-Path -LiteralPath $sys) { $crt = Get-Item -LiteralPath $sys }
}
if ($crt) {
    Write-Host "附带 VC 运行库: $($crt.FullName)"
    Copy-Item -LiteralPath $crt.FullName -Destination (Join-Path $pkgDir 'vcruntime140.dll') -Force
    $dllCopied = $true
} else {
    Write-Warning "没找到 vcruntime140.dll，未打包。目标机器需自装 VC++ 运行库（x64）。"
}

# ============================================================
# 5. 使用说明
# ============================================================
$readme = @'
zm_emu —— zmaee applet 模拟器（Windows x64 便携版）
======================================================

【怎么运行】
  双击 zm_emu.exe 即可。默认加载 applet\00000102（Soundboard 声音板）。

  想跑其它 applet，在命令行里执行（把 00000102 换成其它目录名）：
      zm_emu.exe 00000506

  查看全部命令行选项：
      zm_emu.exe --help

【系统要求】
  Windows 10 / 11（64 位）

【目录说明】（请勿打散或移动，程序按相对路径找资源）
  zm_emu.exe                        主程序
  applet\                           所有 applet 资源
  src\zmaee\unifont_t-18.0.01.pcf   自带像素字体（Unifont，含全部汉字字形）
  vcruntime140.dll                  Microsoft VC++ 运行库（已附带，免安装）
  使用说明.txt                       本文件

【常见问题】
  * 画面正常但没有文字？
      确认 src\ 目录还在、且没被移动。程序找不到自带字体时会退回系统字体。
  * 提示缺少 dll？
      本包已带 vcruntime140.dll；若仍报错，可安装微软官方
      "Visual C++ Redistributable (x64)"。
  * 日志中文乱码？
      程序已自动把控制台切到 UTF-8；若仍乱码，改用 Windows Terminal 打开，
      或先执行一次 chcp 65001。
  * 解压后报错/文件不全？
      请用 Windows 10/11 自带解压或 7-Zip 解压（路径较深时避免用老解压工具）。

【操作提示】
  鼠标点击 = 触摸；关窗口退出。
'@
# 说明文件写成 UTF-8 with BOM，Windows 记事本打开才不会乱码
$readmePath = Join-Path $pkgDir '使用说明.txt'
[System.IO.File]::WriteAllText($readmePath, $readme, (New-Object System.Text.UTF8Encoding $true))

# ============================================================
# 6. 压缩
# ============================================================
if ($NoZip) {
    Write-Host ""
    Write-Host "已生成目录（未压缩）: $pkgDir"
} else {
    $zipPath = Join-Path $ScriptDir (Join-Path $OutDir "$Name.zip")
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }

    Write-Host "正在压缩（约 100MB，需要一两分钟）..."
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    # includeBaseDirectory = $true → zip 里带顶层文件夹名，解压后是一个整目录
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $pkgDir, $zipPath,
        [System.IO.Compression.CompressionLevel]::Optimal, $true)

    $mb = [Math]::Round((Get-Item -LiteralPath $zipPath).Length / 1MB, 1)
    Write-Host ""
    Write-Host "打包完成: $zipPath（$mb MB）"
}

if (-not $dllCopied) { Write-Host "注意: 未附带 vcruntime140.dll（见上方警告）。" }
Write-Host "把这个 zip 发给朋友，解压后双击 zm_emu.exe 即可运行。"

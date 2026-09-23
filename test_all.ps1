#Requires -Version 5.1
<#
.SYNOPSIS
    zm_emu 批量稳定性测试 —— Windows / PowerShell 版
    （与 test_all.sh 语义完全对齐，只是把 bash / timeout / grep 换成 PowerShell 实现）

.DESCRIPTION
    逐个 applet 跑固定时长，统计：稳定 / 崩溃 / 早退，并打印崩溃点(err/PC)。

    判定:
      稳定 = 跑满时长且日志无"异常停止"
      崩溃 = 日志出现"异常停止"
      早退 = 未跑满、也未崩溃（自身提前退出）
      退出 = 不限时长模式下 applet 自己退出且未崩溃

    注：目录名与 .app 名可能不同（如 000004051/00000405.app），
        故对 zm_emu 传 .app 的**全路径**而非短名。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\test_all.ps1
    # 跑 applet/ 下所有带 .app 的目录（每个 6s）

.EXAMPLE
    $env:ZM_TEST_SEC=30; powershell -ExecutionPolicy Bypass -File .\test_all.ps1
    # 改单次时长为 30s

.EXAMPLE
    $env:ZM_TEST_SEC=0; powershell -ExecutionPolicy Bypass -File .\test_all.ps1
    # 不限时长（跑到 applet 自己退出，Ctrl-C 结束）

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\test_all.ps1 000004fe 00000506
    # 只测指定目录名

.EXAMPLE
    $env:ZM_TEST_DISPLAY=1; powershell -ExecutionPolicy Bypass -File .\test_all.ps1
    # 用真实窗口（默认无头 SDL dummy，不弹窗）

.NOTES
    环境变量:
      ZM_TEST_SEC      每个 applet 运行秒数（默认 6；填 0 / inf / none 表示不限时长）
      ZM_TEST_OUT      日志输出目录（默认 %TEMP%\zm_test）
      ZM_TEST_DISPLAY  置 1 则用真实窗口（默认只把**画面**设成 dummy，音频照常出声）
      ZM_TEST_MUTE     置 1 才把音频驱动设成 dummy（默认不静音）

    若提示"禁止运行脚本"，用上面示例里的 -ExecutionPolicy Bypass 方式启动，
    或先执行一次： Set-ExecutionPolicy -Scope Process Bypass
#>

[CmdletBinding()]
param(
    # 位置参数：只测指定的 applet 目录名（不给则跑全部）
    [Parameter(ValueFromRemainingArguments = $true, Position = 0)]
    [string[]]$Names
)

$ErrorActionPreference = 'Stop'

# ============================================================
# 0. 控制台编码
#    结果里全是中文（稳定/崩溃/早退），且日志是 UTF-8，
#    不设的话 PowerShell 5.1 会按 GBK 输出/读取，中文变乱码。
# ============================================================
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$OutputEncoding = [System.Text.Encoding]::UTF8

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $ScriptDir

# 交互控制台下用 `r 回到行首做"同一行动态刷新"（与 bash 版 printf '\r' 一致）；
# 输出被重定向/管道捕获时 CR 不会真的回车，只会留下残字符让日志串行，
# 所以那种情况下老老实实每行一条结果。
$UseCR = $true
try { $UseCR = -not [Console]::IsOutputRedirected } catch { $UseCR = $true }

# ============================================================
# 1. 先构建（对应 bash 版开头的 xmake）
#    xmake 可能没进 PATH（例如装在 %USERPROFILE%\xmake），这里做个兜底查找。
# ============================================================
$xmakeCmd = Get-Command xmake -ErrorAction SilentlyContinue
if ($xmakeCmd) {
    $xmakeExe = $xmakeCmd.Source
} else {
    $cand = Join-Path $env:USERPROFILE 'xmake\xmake.exe'
    if (Test-Path -LiteralPath $cand) {
        $xmakeExe = $cand
    } else {
        Write-Error "找不到 xmake。请先安装 xmake，或把它所在目录加入 PATH。"
        exit 1
    }
}
Write-Host "构建中 (xmake) ..."
& $xmakeExe build -y
if ($LASTEXITCODE -ne 0) {
    Write-Error "构建失败（exit=$LASTEXITCODE）"
    exit 1
}

# ============================================================
# 2. 路径与参数
# ============================================================
$BIN = Join-Path $ScriptDir 'build\windows\x64\release\zm_emu.exe'
if (-not (Test-Path -LiteralPath $BIN)) {
    # 架构/模式不同时兜底找一下（如 x86、debug）
    $found = Get-ChildItem -Path (Join-Path $ScriptDir 'build\windows') `
        -Filter 'zm_emu.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($found) { $BIN = $found.FullName }
}
if (-not (Test-Path -LiteralPath $BIN)) {
    Write-Error "找不到 $BIN ，先执行: xmake build"
    exit 1
}

$ROOT = Join-Path $ScriptDir 'applet'
if (-not (Test-Path -LiteralPath $ROOT)) {
    Write-Error "找不到 applet 目录: $ROOT"
    exit 1
}

$SEC = if ($env:ZM_TEST_SEC) { $env:ZM_TEST_SEC } else { '6' }
$OUT = if ($env:ZM_TEST_OUT) { $env:ZM_TEST_OUT } else { Join-Path $env:TEMP 'zm_test' }
New-Item -ItemType Directory -Force -Path $OUT | Out-Null

# ============================================================
# 3. 无头运行（避免批量弹窗）
#    注意：**不再默认静音**——出不出声是 applet 自己（ISetting 键 "on"）说了算，
#    把音频驱动设成 dummy 会让"声音开关"看起来永远失效。批量跑嫌吵再设
#    ZM_TEST_MUTE=1。
# ============================================================
$RealDisplay = ($env:ZM_TEST_DISPLAY -eq '1')
if ($RealDisplay) {
    Remove-Item 'Env:\SDL_VIDEODRIVER' -ErrorAction SilentlyContinue
} else {
    $env:SDL_VIDEODRIVER = 'dummy'
}
if ($env:ZM_TEST_MUTE -eq '1') {
    $env:SDL_AUDIODRIVER = 'dummy'
} else {
    Remove-Item 'Env:\SDL_AUDIODRIVER' -ErrorAction SilentlyContinue
}

# ============================================================
# 4. 时长控制
#    ZM_TEST_SEC = 0/inf/none/off/unlimited → 不限时长（不加超时，
#    一直跑到 applet 自己退出）。带事件循环的 applet 此时会一直跑，
#    想结束请 Ctrl-C，或配 ZM_TEST_DISPLAY=1 直接关窗口。
# ============================================================
$Unlimited  = $false
$TimeoutSec = [double]0
switch -Regex ($SEC.ToLowerInvariant()) {
    '^(0|inf|none|off|unlimited)$' {
        $Unlimited = $true
        break
    }
    default {
        if (-not [double]::TryParse($SEC, [ref]$TimeoutSec)) {
            Write-Error "ZM_TEST_SEC 不是数字: $SEC"
            exit 1
        }
    }
}

# ============================================================
# 5. 收集待测 applet 目录名
# ============================================================
function Get-AppNames {
    param([string[]]$Specified)
    if ($Specified -and $Specified.Count -gt 0) {
        return $Specified
    }
    Get-ChildItem -LiteralPath $ROOT -Directory | ForEach-Object {
        $hasApp = Get-ChildItem -LiteralPath $_.FullName -Filter '*.app' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($hasApp) { $_.Name }
    }
}
$appNames = @(Get-AppNames -Specified $Names)
if ($appNames.Count -eq 0) {
    Write-Error "没有找到任何带 .app 的 applet 目录"
    exit 1
}

# ============================================================
# 6. 逐个跑
# ============================================================
Write-Host ("{0,-12} {1,-8} {2,-5} {3,-18} {4}" -f 'applet', '结果', '秒', 'err/PC', '备注')
Write-Host ('-' * 84)

$n_ok = 0; $n_crash = 0; $n_early = 0; $n_exit = 0

try {
    foreach ($name in $appNames) {
        $appFile = Get-ChildItem -LiteralPath (Join-Path $ROOT $name) -Filter '*.app' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if (-not $appFile) { continue }

        $log    = Join-Path $OUT "$name.log"
        $outTmp = "$log.stdout"
        $errTmp = "$log.stderr"

        if ($UseCR) { Write-Host ("  ... 正在跑 {0,-12} " -f $name) -NoNewline }

        # 统一成正斜杠：Windows 的 fopen 同样接受 '/'，这样即使拿到的是旧版
        # （只按 '/' 判断"完整路径 vs 短名"）的二进制也不会误判。
        $env:ZM_APPLET = $appFile.FullName.Replace('\', '/')
        $env:ZM_LOG    = 'error'

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $timedOut = $false

        # stdout / stderr 分开重定向（PowerShell 不能像 bash 的 2>&1 那样合并到同一
        # 文件），跑完再拼回单个 $log，保证和 bash 版一样拿到完整日志。
        # 注意：PowerShell 里 -NoNewWindow 与 -WindowStyle **互斥**，不能同时给。
        # 无头模式隐藏窗口避免闪窗；真实窗口模式(ZM_TEST_DISPLAY=1)要看得见画面。
        $spArgs = @{
            FilePath               = $BIN
            RedirectStandardOutput = $outTmp
            RedirectStandardError  = $errTmp
            PassThru               = $true
        }
        if (-not $RealDisplay) { $spArgs['WindowStyle'] = 'Hidden' }
        $p = Start-Process @spArgs
        try {
            if ($Unlimited) {
                $p.WaitForExit()
            } else {
                $ms = [int][Math]::Round($TimeoutSec * 1000)
                if (-not $p.WaitForExit($ms)) {
                    $timedOut = $true
                    # bash 版用 timeout -k 2：先 SIGTERM（SDL 会转成 SDL_QUIT），
                    # 2 秒后仍不死再 SIGKILL。Windows 没有 SIGTERM，
                    # 这里先尝试优雅关窗，失败/无窗口则直接强杀。
                    $closed = $false
                    try { $closed = $p.CloseMainWindow() } catch { }
                    if ($closed) {
                        if (-not $p.WaitForExit(2000)) { $p.Kill() }
                    } else {
                        $p.Kill()
                    }
                    $p.WaitForExit(5000) | Out-Null
                }
            }
        } finally {
            if (-not $p.HasExited) { try { $p.Kill() } catch { } }
        }
        $sw.Stop()
        $dur = [int][Math]::Round($sw.Elapsed.TotalSeconds)
        $rc  = $p.ExitCode

        # ---- 合并日志（字节级拼接，避免 PowerShell 重定向改写编码）----
        $outBytes = if (Test-Path -LiteralPath $outTmp) { [System.IO.File]::ReadAllBytes($outTmp) } else { @() }
        $errBytes = if (Test-Path -LiteralPath $errTmp) { [System.IO.File]::ReadAllBytes($errTmp) } else { @() }
        $fs = [System.IO.File]::Create($log)
        try {
            if ($outBytes.Count) { $fs.Write($outBytes, 0, $outBytes.Count) }
            if ($errBytes.Count) { $fs.Write($errBytes, 0, $errBytes.Count) }
        } finally { $fs.Dispose() }
        Remove-Item -LiteralPath $outTmp, $errTmp -ErrorAction SilentlyContinue

        # ---- 提取崩溃点 ----
        # 日志是 UTF-8 字节流，这里按 UTF-8 解码后正则匹配（不依赖 Select-String
        # 的默认编码，PowerShell 5.1 默认按 GBK/ANSI 读会匹配不到中文）。
        $err = ''
        $pc  = ''
        if ((Test-Path -LiteralPath $log) -and ((Get-Item -LiteralPath $log).Length -gt 0)) {
            $text = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($log))
            if ($text -match '异常停止') {
                $m = [regex]::Match($text, 'err=(\d+)')
                if ($m.Success) { $err = $m.Groups[1].Value }
                $m2 = [regex]::Match($text, 'R15=0x([0-9A-Fa-f]+)')
                if ($m2.Success) { $pc = $m2.Groups[1].Value }
            }
        }

        # ---- 判定（与 bash 版一致）----
        if ($err -ne '') {
            $n_crash++
            $line = "{0,-12} {1,-8} {2,-5} {3,-18} {4}" -f $name, '崩溃', $dur, "err=$err pc=0x$pc", $log
        } elseif ($Unlimited) {
            $n_exit++
            $line = "{0,-12} {1,-8} {2,-5} {3,-18} {4}" -f $name, '退出', $dur, '-', "exit=$rc"
        } elseif ($timedOut) {
            # 对应 bash 里 timeout 的 rc=124：跑满时长 = 稳定
            $n_ok++
            $line = "{0,-12} {1,-8} {2,-5} {3,-18} {4}" -f $name, '稳定', $dur, '-', "跑满 ${SEC}s"
        } else {
            $n_early++
            $line = "{0,-12} {1,-8} {2,-5} {3,-18} {4}" -f $name, '早退', $dur, "exit=$rc", $log
        }
        if ($UseCR) { Write-Host "`r$line" } else { Write-Host $line }
    }
} finally {
    # 清理本脚本设置的环境变量，避免污染当前会话
    Remove-Item 'Env:\ZM_APPLET' -ErrorAction SilentlyContinue
    Remove-Item 'Env:\ZM_LOG'    -ErrorAction SilentlyContinue
}

# ============================================================
# 7. 汇总
# ============================================================
Write-Host ''
if ($Unlimited) {
    Write-Host "汇总(不限时长): 退出 $n_exit | 崩溃 $n_crash | 共 $($n_exit + $n_crash)"
} else {
    Write-Host "汇总: 稳定 $n_ok | 崩溃 $n_crash | 早退 $n_early | 共 $($n_ok + $n_crash + $n_early)"
}
Write-Host "日志: $OUT\<applet>.log"

if ($n_crash -gt 0) { exit 1 } else { exit 0 }

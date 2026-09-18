# EHE 目标机测试脚本（一键跑完并把结果落成一份报告）
#
# 用途：在目标机（例：i5-1135G7 / Intel Iris Xe 核显）上采集 DESIGN §6 的验证数据。
#   设计约束（DESIGN §6 L4）：GPU 渲染刻意不进 CI，所以 GPU 结论只能在目标机上采集，
#   本脚本负责把采集结果落成**一个文件**，便于回传核对。
#
# ---------------------------------------------------------------------------
# 编码要求（踩过三次坑，务必遵守）
#   1. 本文件必须保存为 **UTF-8 带 BOM**。Windows PowerShell 5.1 对**无 BOM 的 .ps1**
#      按系统代码页（中文为 GBK）解释，脚本里的中文会错乱，直接解析失败，报
#      「表达式或语句中包含意外的标记"}"」/「字符串缺少终止符」。
#   2. 本文件内**不允许混入非 UTF-8 字节**。用编辑器或以 PowerShell 改写时若编码判断有误，
#      会把部分中文写成 GBK 字节，形成"混合编码"文件——BOM 正常但个别字符串仍是乱码。
#   3. 以上两条有 CI 断言兜底：tests/src/test_repo_hygiene.cpp
#      （检查 BOM + 严格 UTF-8 解码，任何非 UTF-8 字节都会让 CI 失败）
# ---------------------------------------------------------------------------
#
# 用法（在包根目录，即解压后含 ehe.exe 的目录）：
#   powershell -ExecutionPolicy Bypass -File .\run_target_tests.ps1
#   可选：
#     -TimeoutSec 1800     单个 512x512 抹烟步骤的超时（iGPU 上可能要几十秒到几分钟）
#     -TryMixed            额外尝试 mixed(fp64) 模式（Intel 核显无原生 fp64，可能极慢/无响应，默认跳过）
#     -SkipLarge           跳过 512x512 步骤（先快速拿小尺寸数据）
#     -Windowed            额外跑一次窗口模式 3 帧（会短暂弹出窗口）
#
# 产出：.\target_report\  目录下
#   target_report.txt    唯一需要回传的文件（含环境信息 + 每一步的完整输出与耗时）
#   *.pfm / *.png        各尺寸抹烟产出的 HDR 基准与预览图
#
# 退出码：0 = 关键步骤全部通过；1 = 有步骤失败或超时（详见报告）

param(
    [string]$PackageRoot = $PSScriptRoot,
    [string]$OutDir = "",
    [int]$TimeoutSec = 1800,
    [switch]$TryMixed,
    [switch]$SkipLarge,
    [switch]$Windowed
)

# 控制台切成 UTF-8：程序与脚本输出的中文在中文 Windows（GBK 代码页）下才不乱码
try {
    chcp 65001 | Out-Null
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
    $OutputEncoding = [Text.Encoding]::UTF8
} catch {
    # 非交互宿主可能不支持，忽略
}

$ErrorActionPreference = "Continue"
if ([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = Join-Path $PackageRoot "target_report" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$reportPath = Join-Path $OutDir "target_report.txt"
$failures = 0
# UTF-8 无 BOM：生成的参数文件要喂给 C++ 的 JSON 解析器（那边已加 BOM 容错，写干净更好）
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Report([string]$text) {
    Write-Host $text
    Add-Content -Path $reportPath -Value $text -Encoding UTF8
}

function Section([string]$title) {
    Write-Report ""
    Write-Report ("=" * 78)
    Write-Report "## $title"
    Write-Report ("=" * 78)
}

# 运行一个命令并记录：stdout/stderr 全部进报告，返回对象含 Code/Seconds/TimedOut
#
# 实现说明（踩坑记录）：**不能用 Start-Process -PassThru 配合 Wait-Process**——
#   Windows PowerShell 5.1 下不带 -Wait 启动的进程读不到 ExitCode（读出为空），
#   会让每一步的退出码都变成"失败"。这里改用 .NET Process 直控：
#     - WaitForExit(ms) 提供超时控制；
#     - 标准输出/错误必须异步读（ReadToEndAsync），否则子进程写满管道缓冲区会死锁；
#     - 显式 StandardOutputEncoding = UTF-8，程序输出的中文才不会乱码。
function Invoke-Step {
    param(
        [string]$Title,
        [string]$Exe,
        [string[]]$Arguments,
        [int]$Timeout = 300,
        [string]$Tag = "step"
    )
    Section "$Title"
    Write-Report ("命令: `"$Exe`" " + ($Arguments -join " "))

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $quoted = @()
    foreach ($arg in $Arguments) {
        if ($arg -match '\s') { $quoted += ('"' + $arg + '"') } else { $quoted += $arg }
    }
    $psi.Arguments = ($quoted -join ' ')
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $utf8NoBom
    $psi.StandardErrorEncoding = $utf8NoBom

    $clock = [Diagnostics.Stopwatch]::StartNew()
    $proc = [System.Diagnostics.Process]::Start($psi)
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()

    $timedOut = $false
    if (-not $proc.WaitForExit($Timeout * 1000)) {
        $timedOut = $true
        try { $proc.Kill() } catch { }
        $proc.WaitForExit()
    } else {
        $proc.WaitForExit()
    }
    $clock.Stop()

    $stdoutText = ""
    $stderrText = ""
    try { $stdoutText = $outTask.Result } catch { }
    try { $stderrText = $errTask.Result } catch { }

    [IO.File]::WriteAllText((Join-Path $OutDir "$Tag.out.txt"), $stdoutText, $utf8NoBom)
    [IO.File]::WriteAllText((Join-Path $OutDir "$Tag.err.txt"), $stderrText, $utf8NoBom)

    $code = if ($timedOut) { "TIMEOUT" } else { $proc.ExitCode }
    Write-Report "耗时: $([math]::Round($clock.Elapsed.TotalSeconds,1)) s    退出码: $code"

    foreach ($entry in @(
        @{ Name = "$Tag.out.txt"; Text = $stdoutText },
        @{ Name = "$Tag.err.txt"; Text = $stderrText })) {
        if (-not [string]::IsNullOrWhiteSpace($entry.Text)) {
            Write-Report "--- $($entry.Name) ---"
            foreach ($line in ($entry.Text -split "`r?`n")) {
                if ($line -ne "") { Write-Report $line }
            }
        }
    }

    return [pscustomobject]@{
        Code = $code
        Seconds = [math]::Round($clock.Elapsed.TotalSeconds,1)
        TimedOut = $timedOut
    }
}

# ---------------------------------------------------------------- 0. 环境信息
Section "0. 环境信息（目标机）"
try {
    $os = Get-CimInstance Win32_OperatingSystem
    Write-Report "OS        : $($os.Caption) $($os.Version)"
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    Write-Report "CPU       : $($cpu.Name)（$($cpu.NumberOfCores) 核 / $($cpu.NumberOfLogicalProcessors) 线程）"
    Write-Report "内存      : $([math]::Round($os.TotalVisibleMemorySize/1MB,1)) GB"
    Write-Report "PowerShell: $($PSVersionTable.PSVersion)（$($PSVersionTable.PSEdition)）"
    foreach ($gpu in (Get-CimInstance Win32_VideoController)) {
        Write-Report "GPU       : $($gpu.Name)  驱动 $($gpu.DriverVersion)  日期 $($gpu.DriverDate)  显存(报称) $([math]::Round($gpu.AdapterRAM/1MB,0)) MB"
    }
} catch {
    Write-Report "环境信息采集失败: $($_.Exception.Message)"
}
Write-Report "脚本参数  : TimeoutSec=$TimeoutSec TryMixed=$($TryMixed.IsPresent) SkipLarge=$($SkipLarge.IsPresent) Windowed=$($Windowed.IsPresent)"
Write-Report "包根目录  : $PackageRoot"

$exe = Join-Path $PackageRoot "ehe.exe"
$shaders = Join-Path $PackageRoot "shaders"
$goldenDir = Join-Path $PackageRoot "tests\golden"
if (-not (Test-Path $exe)) {
    Write-Report "找不到 ehe.exe：请在解压后的包根目录运行本脚本，或用 -PackageRoot 指定路径"
    exit 1
}
$common = @("--shader-root=$shaders")

# ---------------------------------------------------------------- 1. 能力探测
$caps = Invoke-Step -Title "1. 能力探测（--caps：API 版本/设备/扩展/精度可编译性与帧耗时）" -Exe $exe `
    -Arguments (@("--caps", "--config=$goldenDir\params_tiny.json") + $common) -Timeout 300 -Tag "caps"

if ($TryMixed) {
    $capsMixed = Invoke-Step -Title "1b. 能力探测（--time-mixed：额外实测 fp64 帧耗时，可能极慢）" -Exe $exe `
        -Arguments (@("--caps", "--time-mixed", "--config=$goldenDir\params_tiny.json") + $common) -Timeout $TimeoutSec -Tag "caps_mixed"
} else {
    Section "1b. 能力探测（fp64 实测）"
    Write-Report "已跳过：Intel 核显等无原生 fp64 的 GPU 上 fp64 由驱动模拟，实测可能挂死驱动。"
    Write-Report "如确需数据，用 -TryMixed 重跑（并把 -TimeoutSec 设大）。"
}

# ---------------------------------------------------------------- 2. 小尺寸抹烟（跑得动才能上大图）
$sizes = @(
    @{ Tag = "tiny";  Cfg = "params_tiny.json";  Note = "32x32, n_max=60" },
    @{ Tag = "small"; Cfg = "params_small.json"; Note = "64x64, n_max=100" }
)
foreach ($size in $sizes) {
    $cfg = Join-Path $goldenDir $size.Cfg
    # 目标机上强制 fp32（参数文件若为 mixed，会因 fp64 模拟而极慢）
    $fp32Cfg = Join-Path $OutDir "params_fp32_$($size.Tag).json"
    $json = [IO.File]::ReadAllText($cfg)
    $json = $json -replace '"precision"\s*:\s*"mixed"', '"precision": "fp32"'
    [IO.File]::WriteAllText($fp32Cfg, $json, $utf8NoBom)
    $shot = Join-Path $OutDir "shot_$($size.Tag)"
    $title = "2. 抹烟 " + $size.Note + "（fp32）→ NMSE 差分"
    $result = Invoke-Step -Title $title -Exe $exe `
        -Arguments (@("--smoke", "--frames=1", "--shot=$shot", "--config=$fp32Cfg",
                      "--golden=$goldenDir\golden_$($size.Tag).pfm") + $common) -Timeout 600 -Tag "smoke_$($size.Tag)"
    if ($result.Code -ne 0) { $failures++ }
}

# ---------------------------------------------------------------- 3. 512x512 抹烟（DESIGN §6.1 固化参数）
if (-not $SkipLarge) {
    $cfg = Join-Path $goldenDir "params.json"
    $fp32Cfg = Join-Path $OutDir "params_fp32_512.json"
    $json = [IO.File]::ReadAllText($cfg)
    $json = $json -replace '"precision"\s*:\s*"mixed"', '"precision": "fp32"'
    [IO.File]::WriteAllText($fp32Cfg, $json, $utf8NoBom)

    Section "3. 512x512 抹烟（fp32）——首次在该机跑，可能需数十秒到数分钟"
    Write-Report "提示：Iris Xe 一类核显上，512x512、n_max=1000 的 fp32 单帧预计在数十秒量级；"
    Write-Report "      超过 -TimeoutSec（当前 $TimeoutSec s）会判超时，可先用 -SkipLarge 看小尺寸。"
    $shot = Join-Path $OutDir "shot_512_fp32"
    $result = Invoke-Step -Title "3a. 512x512 抹烟（fp32）→ 与 golden.pfm 差分" -Exe $exe `
        -Arguments (@("--smoke", "--frames=1", "--shot=$shot", "--config=$fp32Cfg",
                      "--golden=$goldenDir\golden.pfm") + $common) -Timeout $TimeoutSec -Tag "smoke_512_fp32"
    if ($result.Code -ne 0) { $failures++ }

    if ($TryMixed) {
        $shotMixed = Join-Path $OutDir "shot_512_mixed"
        $result = Invoke-Step -Title "3b. 512x512 抹烟（mixed/fp64，预计极慢）" -Exe $exe `
            -Arguments (@("--smoke", "--frames=1", "--shot=$shotMixed", "--config=$cfg",
                          "--golden=$goldenDir\golden.pfm") + $common) -Timeout $TimeoutSec -Tag "smoke_512_mixed"
        if ($result.Code -ne 0) { $failures++ }
    } else {
        Section "3b. 512x512 抹烟（mixed/fp64）"
        Write-Report "已跳过（-TryMixed 可启用）。"
    }
} else {
    Section "3. 512x512 抹烟"
    Write-Report "已按 -SkipLarge 跳过。"
}

# ---------------------------------------------------------------- 4. 窗口模式
if ($Windowed) {
    $result = Invoke-Step -Title "4. 窗口模式 3 帧（起窗 + raymarch + ImGui 面板）" -Exe $exe `
        -Arguments (@("--width=640", "--height=360", "--frames=3",
                      "--config=$goldenDir\params_tiny.json") + $common) -Timeout 300 -Tag "windowed"
    if ($result.Code -ne 0) { $failures++ }
} else {
    Section "4. 窗口模式"
    Write-Report "已跳过（-Windowed 可启用；会短暂弹出窗口）。"
}

$result = Invoke-Step -Title "5. 后端探测（--try-backends）" -Exe $exe -Arguments @("--try-backends") -Timeout 180 -Tag "backends"
if ($result.Code -ne 0) { $failures++ }

# ---------------------------------------------------------------- 汇总
Section "汇总"
Write-Report "报告文件: $reportPath"
Write-Report "产出文件:"
Get-ChildItem $OutDir -File | ForEach-Object { Write-Report ("  {0,-34} {1,8:N1} KB" -f $_.Name, ($_.Length / 1KB)) }
Write-Report ""
Write-Report "判读要点："
Write-Report "  1) --caps 里 GL/VK 的 API 版本、设备名、rgba16f 附件、以及 fp32/mixed 的「管线就绪」与否"
Write-Report "  2) Vulkan 的 features.shaderFloat64：0 表示该 GPU 无原生 fp64（T1.6 起 VK 侧走 fp32）"
Write-Report "  3) 各尺寸抹烟的 NMSE：应 <= 1e-3（DESIGN 6.2）；fp32 相对 fp64 基线的实测约 1.6e-4（本机参考值）"
Write-Report "  4) 「预热帧耗时（含 GPU 同步）」才是真实单帧成本；不带 finish 的计时会低估约 20 倍"
Write-Report ""
if ($failures -eq 0) {
    Write-Report "全部关键步骤通过（失败数 0）"
    exit 0
}
Write-Report "有 $failures 个步骤失败或超时，请把本报告回传"
exit 1

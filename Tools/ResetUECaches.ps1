<#
.SYNOPSIS
    重置 Unreal Engine 5.8 的缓存（DDC / Intermediate / 着色器缓存）。

.DESCRIPTION
    默认只是**预览**：列出会删什么、各占多大，什么都不动。加 -Apply 才真的删。

    缓存删掉只会让下次启动变慢（重新推导、重新编译着色器），不会丢内容。
    但脚本对"不是缓存"的东西有硬性保护，见下面的 NEVER 列表。

.PARAMETER ProjectPath
    工程目录。**不传就只清理引擎级缓存**（Scope 里的 Project 部分会被跳过）。
    工程级缓存必须在工程还存在的状态下清，所以要么传路径，要么用 -Scope Engine。

.PARAMETER Scope
    Project / Engine / All（默认 All）

.PARAMETER Apply
    真的执行删除。不加就是预览。

.EXAMPLE
    # 先看会删什么
    .\ResetUECaches.ps1 -ProjectPath "D:\MyProject"

.EXAMPLE
    # 确认后执行
    .\ResetUECaches.ps1 -ProjectPath "D:\MyProject" -Apply

.EXAMPLE
    # 工程已经删了，只清引擎级 / 系统级缓存
    .\ResetUECaches.ps1 -Scope Engine -Apply
#>

param(
    [string]$ProjectPath = '',
    [ValidateSet('Project', 'Engine', 'All')]
    [string]$Scope = 'All',
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# 绝对不能删的东西。
# 命中即中止 —— 尤其是 Binaries：MMD2Unreal 是纯二进制插件，DLL 删了没地方找回来。
# ---------------------------------------------------------------------------
$NEVER = @(
    'Binaries',              # 插件的 DLL，尤其 MMD2Unreal / 预编译版
    'Content',               # 关卡、序列、模型、材质
    'Config',                # 工程设置
    'Backups',               # 我们自己的备份
    'MovieRenders',          # 渲染输出
    'Logs',                  # 日志（排查问题要用）
    'Autosaves',             # 自动保存
    'Source',                # 源码
    'Source.disabled'        # 源码（构建时改名的那份）
)

function Get-Targets {
    # Plain array with +=: ArrayList.Add() returns the index, which would leak into the
    # function's output and turn the returned list into a mix of indices and objects.
    $list = @()
    $wantProject = ($Scope -eq 'Project' -or $Scope -eq 'All') -and -not [string]::IsNullOrWhiteSpace($ProjectPath)

    if ($wantProject) {
        $list += [pscustomobject]@{ Kind = 'Project'; Path = Join-Path $ProjectPath 'DerivedDataCache' }
        $list += [pscustomobject]@{ Kind = 'Project'; Path = Join-Path $ProjectPath 'Intermediate' }
        $list += [pscustomobject]@{ Kind = 'Project'; Path = Join-Path $ProjectPath 'Saved\ShaderDebugInfo' }

        # 插件的 Intermediate 是构建中间产物，可以删；
        # 插件的 Binaries 不是，靠 $NEVER 挡住。
        $pluginsDir = Join-Path $ProjectPath 'Plugins'
        if (Test-Path $pluginsDir) {
            Get-ChildItem $pluginsDir -Directory | ForEach-Object {
                $list += [pscustomobject]@{ Kind = 'Plugin'; Path = Join-Path $_.FullName 'Intermediate' }
            }
        }
    }

    if ($Scope -eq 'Engine' -or $Scope -eq 'All') {
        $list += [pscustomobject]@{ Kind = 'Engine'; Path = Join-Path $env:LOCALAPPDATA 'UnrealEngine\Common\DerivedDataCache' }
        $list += [pscustomobject]@{ Kind = 'Engine'; Path = Join-Path $env:ProgramData  'Epic\UnrealEngine\Common\DerivedDataCache' }
        $list += [pscustomobject]@{ Kind = 'Engine'; Path = Join-Path $env:LOCALAPPDATA 'UnrealEngine\Common\Zen' }
        # Windows 自己的 D3D 着色器缓存
        $list += [pscustomobject]@{ Kind = 'System'; Path = Join-Path $env:LOCALAPPDATA 'D3DSCache' }

        # 故意不动 %LOCALAPPDATA%\UnrealBuildTool：
        # 那里混着 BuildConfiguration.xml（用户的构建配置，不是缓存），整目录删会连带重置它。
        # UBT 自己的缓存删不删影响很小，得不偿失。
    }

    return $list
}

function Test-Forbidden([string]$Path) {
    $leaf = Split-Path $Path -Leaf
    foreach ($bad in $NEVER) {
        if ($leaf -eq $bad) { return $bad }
    }
    # 路径里任何一段命中也不行，防止有人把 ProjectPath 指到奇怪的地方
    foreach ($bad in $NEVER) {
        if ($Path -match ('\\' + [regex]::Escape($bad) + '(\\|$)')) { return $bad }
    }
    return $null
}

# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=== Unreal 5.8 缓存重置 ===" -ForegroundColor Cyan
Write-Host ("  工程    : {0}" -f $(if ([string]::IsNullOrWhiteSpace($ProjectPath)) { '(未指定，跳过工程级)' } else { $ProjectPath }))
Write-Host ("  范围    : {0}" -f $Scope)
Write-Host ("  模式    : {0}" -f $(if ($Apply) { '执行删除' } else { '预览（加 -Apply 才真删）' }))
Write-Host ""

if (-not [string]::IsNullOrWhiteSpace($ProjectPath) -and -not (Test-Path $ProjectPath)) {
    Write-Host ("工程目录不存在：{0}" -f $ProjectPath) -ForegroundColor Yellow
    Write-Host "工程级缓存会全部跳过。工程已经删了的话，用 -Scope Engine 只清引擎级。"
    Write-Host ""
}

# 编辑器开着的时候删 DDC / Intermediate 会出各种怪问题
$editor = Get-Process -Name 'UnrealEditor', 'UnrealEditor-Cmd' -ErrorAction SilentlyContinue
if ($editor) {
    Write-Host "检测到编辑器正在运行，请先关掉再执行：" -ForegroundColor Red
    $editor | ForEach-Object { Write-Host ("    {0} (PID {1})" -f $_.ProcessName, $_.Id) }
    Write-Host ""
    if ($Apply) { throw "编辑器还开着，已中止。" }
}

$totalBytes = 0
$totalFiles = 0
$skipped = 0

foreach ($target in (Get-Targets)) {
    $path = $target.Path

    $bad = Test-Forbidden $path
    if ($bad) {
        Write-Host ("  [保护] 跳过 {0}  (命中 NEVER: {1})" -f $path, $bad) -ForegroundColor Red
        $skipped++
        continue
    }

    if (-not (Test-Path $path)) {
        Write-Host ("  [跳过] {0}  (不存在)" -f $path) -ForegroundColor DarkGray
        continue
    }

    $items = Get-ChildItem $path -Recurse -File -ErrorAction SilentlyContinue
    $bytes = ($items | Measure-Object Length -Sum).Sum
    if (-not $bytes) { $bytes = 0 }
    $count = ($items | Measure-Object).Count

    $totalBytes += $bytes
    $totalFiles += $count

    $sizeText = '{0:N1} MiB' -f ($bytes / 1MB)
    Write-Host ("  [{0,-7}] {1,-60} {2,10}  ({3} 文件)" -f $target.Kind, $path, $sizeText, $count)

    if ($Apply) {
        try {
            Remove-Item $path -Recurse -Force -ErrorAction Stop
            Write-Host ("            已删除") -ForegroundColor Green
        } catch {
            Write-Host ("            删除失败: {0}" -f $_.Exception.Message) -ForegroundColor Red
        }
    }
}

Write-Host ""
Write-Host ("合计 {0:N1} MiB / {1} 个文件" -f ($totalBytes / 1MB), $totalFiles)
if ($skipped -gt 0) {
    Write-Host ("{0} 个目标被 NEVER 列表保护跳过" -f $skipped) -ForegroundColor Yellow
}

if (-not $Apply) {
    Write-Host ""
    Write-Host "这只是预览。确认无误后加 -Apply 执行：" -ForegroundColor Cyan
    $rerun = "{0} -Scope {1}" -f $MyInvocation.MyCommand.Path, $Scope
    if (-not [string]::IsNullOrWhiteSpace($ProjectPath)) { $rerun += " -ProjectPath '$ProjectPath'" }
    Write-Host ("    {0} -Apply" -f $rerun)
} else {
    Write-Host ""
    Write-Host "完成。下次打开工程会重新推导 DDC、重新编译着色器，第一次会比较慢。" -ForegroundColor Green
}
Write-Host ""

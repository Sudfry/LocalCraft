<#
.SYNOPSIS
    往 CHANGELOG.md 里记一条改动。

.DESCRIPTION
    两个用途：

    1）平时记改动 —— 往「[未发布]」区块里追加条目。
    2）发版本时收口 —— 把「[未发布]」整体变成一个新版本号，并重新开一个空的「[未发布]」。

    分类固定为五个：新增 / 变更 / 修复 / 移除 / 安全。
    同一个分类下多次调用会继续往里加，不会重复建标题。

.PARAMETER Added
    新增了什么功能。

.PARAMETER Changed
    改了哪些已有行为（用户能感知到的）。

.PARAMETER Fixed
    修了哪些 bug。

.PARAMETER Removed
    删掉了什么。

.PARAMETER Security
    与安全相关的改动。

.PARAMETER Release
    发版本。把当前「[未发布]」收口成指定版本号，日期取今天。

.PARAMETER Date
    配合 -Release 指定日期（默认今天，格式 yyyy-MM-dd）。

.PARAMETER Show
    只显示当前「[未发布]」里有什么，不改文件。

.EXAMPLE
    .\记录更新.ps1 -Added "支持拖拽文件夹"

.EXAMPLE
    .\记录更新.ps1 -Added "暗色主题","拖拽排序" -Fixed "200% 缩放下窗口跑出屏幕"

.EXAMPLE
    .\记录更新.ps1

    不带参数运行会进入交互模式，逐个分类问。

.EXAMPLE
    .\记录更新.ps1 -Release 1.5.0

    把「[未发布]」收口成 1.5.0。
#>
[CmdletBinding()]
param(
    [string[]]$Added    = @(),
    [string[]]$Changed  = @(),
    [string[]]$Fixed    = @(),
    [string[]]$Removed  = @(),
    [string[]]$Security = @(),
    [string]$Release = "",
    [string]$Date    = "",
    [switch]$Show
)

$ErrorActionPreference = "Continue"

# ---------------------------------------------------------------- 定位文件
$Here   = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$ChangeLog = Join-Path $Here "CHANGELOG.md"

if (-not (Test-Path $ChangeLog)) {
    Write-Host "找不到 $ChangeLog" -ForegroundColor Red
    exit 1
}

# 分类顺序固定，渲染出来才稳定
$Order   = @("新增", "变更", "修复", "移除", "安全")
$Unreleased = "[未发布]"

$Utf8NoBom = New-Object System.Text.UTF8Encoding $false

function Read-All  { [System.IO.File]::ReadAllText($ChangeLog, [System.Text.Encoding]::UTF8) }
function Write-All($t) { [System.IO.File]::WriteAllText($ChangeLog, $t, $Utf8NoBom) }

# ---------------------------------------------------------------- 解析 [未发布]
# 返回 @{ 分类名 = @(条目...) }
function Parse-Unreleased($text) {
    $map = @{}
    $lines = $text -split "`r?`n"

    $inBlock = $false
    $cur = $null
    foreach ($ln in $lines) {
        if ($ln -match '^##\s+\[' + [regex]::Escape('未发布') + '\]') { $inBlock = $true; continue }
        if ($inBlock -and $ln -match '^##\s+\[') { break }      # 到了下一个版本
        if ($inBlock -and $ln -match '^---\s*$') { break }      # 区块结束
        if (-not $inBlock) { continue }

        if ($ln -match '^###\s+(.+?)\s*$') { $cur = $matches[1].Trim(); if (-not $map.ContainsKey($cur)) { $map[$cur] = @() }; continue }
        if ($cur -and $ln -match '^\s*-\s+(.+?)\s*$') { $map[$cur] += $matches[1].Trim() }
    }
    return $map
}

# ---------------------------------------------------------------- 渲染 [未发布]
function Render-Unreleased($map) {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("## $Unreleased")
    [void]$sb.AppendLine()

    $any = $false
    foreach ($cat in $Order) {
        if ($map.ContainsKey($cat) -and $map[$cat].Count -gt 0) {
            $any = $true
            [void]$sb.AppendLine("### $cat")
            [void]$sb.AppendLine()
            foreach ($item in $map[$cat]) { [void]$sb.AppendLine("- $item") }
            [void]$sb.AppendLine()
        }
    }
    # 没内容时留一句提示，免得新人不知道这里该写什么
    if (-not $any) {
        [void]$sb.AppendLine("<!-- 新改动写在这里。发版本时整体挪到下面，改成版本号和日期。 -->")
        [void]$sb.AppendLine()
    }
    return $sb.ToString().TrimEnd("`r", "`n")
}

# 把文本里 [未发布] 整块替换掉
function Replace-Unreleased($text, $rendered) {
    $lines = $text -split "`r?`n"
    $out = New-Object System.Collections.Generic.List[string]
    $i = 0
    $done = $false
    while ($i -lt $lines.Count) {
        if (-not $done -and $lines[$i] -match '^##\s+\[' + [regex]::Escape('未发布') + '\]') {
            $out.Add($rendered)
            $out.Add("")            # 区块与后面的 --- 之间留一个空行
            # 跳过旧的整块，直到 '---'
            $j = $i + 1
            while ($j -lt $lines.Count -and $lines[$j] -notmatch '^---\s*$') { $j++ }
            $i = $j          # 让循环从 '---' 那行继续
            $done = $true
            continue
        }
        $out.Add($lines[$i])
        $i++
    }
    return ($out -join "`r`n")
}

$text = Read-All
$map  = Parse-Unreleased $text

# ---------------------------------------------------------------- -Show
if ($Show) {
    Write-Host ""
    Write-Host "  CHANGELOG 里 [未发布] 的内容：" -ForegroundColor Cyan
    $n = 0
    foreach ($cat in $Order) {
        if ($map.ContainsKey($cat) -and $map[$cat].Count -gt 0) {
            Write-Host "    $cat" -ForegroundColor White
            foreach ($it in $map[$cat]) { Write-Host "      - $it"; $n++ }
        }
    }
    if ($n -eq 0) { Write-Host "    （空的）" -ForegroundColor Gray }
    Write-Host ""
    exit 0
}

# ---------------------------------------------------------------- -Release
if ($Release) {
    if ($Release -notmatch '^\d+\.\d+\.\d+$') {
        Write-Host "版本号格式应为 x.y.z，例如 1.5.0" -ForegroundColor Red
        exit 1
    }
    $total = 0
    foreach ($cat in $Order) { if ($map.ContainsKey($cat)) { $total += $map[$cat].Count } }
    if ($total -eq 0) {
        Write-Host "[未发布] 里是空的，没什么可发的。" -ForegroundColor Yellow
        Write-Host "先用 .\记录更新.ps1 -Added `"...`" 记几条。" -ForegroundColor Gray
        exit 1
    }

    if (-not $Date) { $Date = Get-Date -Format "yyyy-MM-dd" }

    # 把当前 [未发布] 渲染成正式区块
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("## [$Release] — $Date")
    [void]$sb.AppendLine()
    foreach ($cat in $Order) {
        if ($map.ContainsKey($cat) -and $map[$cat].Count -gt 0) {
            [void]$sb.AppendLine("### $cat")
            [void]$sb.AppendLine()
            foreach ($it in $map[$cat]) { [void]$sb.AppendLine("- $it") }
            [void]$sb.AppendLine()
        }
    }
    $released = $sb.ToString().TrimEnd("`r", "`n")

    $empty = @{}
    $newBlock = (Render-Unreleased $empty) + "`r`n`r`n---`r`n`r`n" + $released

    Write-All (Replace-Unreleased $text $newBlock)
    Write-Host ""
    Write-Host "  已发布 [$Release] — $Date（共 $total 条）" -ForegroundColor Green
    Write-Host ""
    exit 0
}

# ---------------------------------------------------------------- 交互模式
$groups = @{
    "新增" = $Added; "变更" = $Changed; "修复" = $Fixed;
    "移除" = $Removed; "安全" = $Security
}
$anyGiven = $false
foreach ($k in $Order) { if ($groups[$k].Count -gt 0) { $anyGiven = $true } }

if (-not $anyGiven) {
    Write-Host ""
    Write-Host "  记录一条更新到 CHANGELOG.md" -ForegroundColor Cyan
    Write-Host "  ------------------------------------------------"
    Write-Host "  每个分类可以写多条，用逗号隔开；直接回车跳过。" -ForegroundColor Gray
    Write-Host ""
    foreach ($cat in $Order) {
        $ans = Read-Host "  $cat"
        if ($ans -and $ans.Trim()) {
            $groups[$cat] = @($ans -split '[,，]' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        }
    }
    Write-Host ""
}

# ---------------------------------------------------------------- 合并写入
$addedCount = 0
foreach ($cat in $Order) {
    $items = $groups[$cat]
    if (-not $items -or $items.Count -eq 0) { continue }
    if (-not $map.ContainsKey($cat)) { $map[$cat] = @() }
    foreach ($it in $items) {
        if ($it -and $map[$cat] -notcontains $it) { $map[$cat] += $it; $addedCount++ }
    }
}

if ($addedCount -eq 0) {
    Write-Host "  没有新内容，文件未改动。" -ForegroundColor Yellow
    exit 0
}

Write-All (Replace-Unreleased $text (Render-Unreleased $map))

Write-Host "  已写入 $addedCount 条到 [未发布]" -ForegroundColor Green
Write-Host "  $ChangeLog" -ForegroundColor Gray
Write-Host ""

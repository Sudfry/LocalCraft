<#
.SYNOPSIS
    把当前版本完整打包归档。

.DESCRIPTION
    每个版本存一套完整的东西：**源码 + 可执行程序 + 项目介绍 + 更新日志 + 教程 +
    版本信息（含哈希）**，以后要翻旧版本随时能翻出来。

    产出结构：

        发布/
        ├── v1.4.0/
        │   ├── LocalCraft-1.4.0-源码.zip      整个源码树
        │   ├── LocalCraft-1.4.0-程序.zip      可直接运行的 exe + dll + 说明
        │   ├── README.md                      本版本的项目介绍（快照）
        │   ├── CHANGELOG.md                   本版本的更新日志（快照）
        │   ├── 教程.md                        使用教程（快照）
        │   ├── 版本信息.txt                   版本号、日期、体积、SHA256
        │   └── 源码目录.txt                   源码包里都有什么
        └── v1.4.0.zip                         上面整个目录再打一个包

    版本号不是手填的，直接从 src/app.rc 里读，避免和程序本身对不上。

.PARAMETER Version
    强制指定版本号。不填则从 src/app.rc 读。

.PARAMETER Force
    该版本已经归档过时覆盖它。不加则拒绝覆盖，防止手滑。

.PARAMETER NoOuterZip
    不生成最外层的 v1.4.0.zip（只要目录时用）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File 打包版本.ps1

.EXAMPLE
    # 看看会打哪些东西，不真的写文件
    powershell -ExecutionPolicy Bypass -File 打包版本.ps1 -DryRun
#>
[CmdletBinding()]
param(
    [string]$Version = "",
    [switch]$Force,
    [switch]$NoOuterZip,
    [switch]$DryRun
)

$ErrorActionPreference = "Continue"

$Root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$SrcDir  = Join-Path $Root "src"
$DistDir = Join-Path $Root "dist"
$OutBase = Join-Path $Root "发布"

function Step($t) { Write-Host ""; Write-Host "  $t" -ForegroundColor Cyan }
function Ok($t)   { Write-Host "    $t" -ForegroundColor Green }
function Info($t) { Write-Host "    $t" -ForegroundColor Gray }
function Warn($t) { Write-Host "    $t" -ForegroundColor Yellow }
function Die($t)  { Write-Host ""; Write-Host "  错误：$t" -ForegroundColor Red; exit 1 }
function Mb($n)   { return [Math]::Round($n / 1MB, 2) }

Write-Host ""
Write-Host "  LocalCraft 版本归档" -ForegroundColor White
Write-Host "  =================================================="

# ---------------------------------------------------------------- 1. 版本号
Step "1/6  确定版本号"

if (-not $Version) {
    # 从 app.rc 的 FILEVERSION 读，例如 "FILEVERSION     1,4,0,0"
    $rc = Join-Path $SrcDir "app.rc"
    if (-not (Test-Path $rc)) { Die "找不到 src\app.rc，无法确定版本号" }
    $m = Select-String -Path $rc -Pattern '^\s*FILEVERSION\s+(\d+),(\d+),(\d+),(\d+)' |
         Select-Object -First 1
    if (-not $m) { Die "src\app.rc 里没有 FILEVERSION，用 -Version 手动指定" }
    $Version = "$($m.Matches[0].Groups[1].Value).$($m.Matches[0].Groups[2].Value).$($m.Matches[0].Groups[3].Value)"
    Info "从 src\app.rc 读到版本号：$Version"
} else {
    Info "使用指定的版本号：$Version"
}

if ($Version -notmatch '^\d+\.\d+\.\d+$') { Die "版本号格式应为 x.y.z，当前是 '$Version'" }

$Tag     = Join-Path $OutBase "v$Version"
$OuterZip = Join-Path $OutBase "v$Version.zip"

if ((Test-Path $Tag) -and -not $Force -and -not $DryRun) {
    Write-Host ""
    Warn "发布\v$Version 已经存在了。"
    Warn "想重新打一遍就加 -Force（会覆盖旧的归档）。"
    Write-Host ""
    exit 1
}

Ok "归档目录：发布\v$Version"

# ---------------------------------------------------------------- 2. 检查素材
Step "2/6  检查要打包的内容"

$need = @(
    @{ p = (Join-Path $Root "README.md");    n = "README.md（项目介绍）" },
    @{ p = (Join-Path $Root "CHANGELOG.md"); n = "CHANGELOG.md（更新日志）" },
    @{ p = (Join-Path $Root "教程.md");       n = "教程.md" },
    @{ p = (Join-Path $DistDir "LocalCraft.exe");      n = "dist\LocalCraft.exe" },
    @{ p = (Join-Path $DistDir "WebView2Loader.dll"); n = "dist\WebView2Loader.dll" }
)
$missing = @()
foreach ($x in $need) { if (-not (Test-Path $x.p)) { $missing += $x.n } }
if ($missing.Count) {
    Warn "以下内容还没准备好："
    $missing | ForEach-Object { Warn "    $_" }
    Die "先补齐再打包（教程.md 不存在就先建一个）"
}
$need | ForEach-Object { Ok $_.n }

# 源码文件清单
# 源码文件清单。
#
# 排除规则值得说一句：build\ 下除了 build.ps1 之外**全都要排掉** ——
# 那里是编译产物（obj\*.o、app.res、另拷一份的 exe/dll）和测试残留
# （zipwork\、testtmp\）。早期只排了 build\obj\，结果源码包里混进了
# 82 个构建文件（含一份 1.1MB 的 exe），源码包从 1.5MB 虚胖到 7MB。
$srcFiles = Get-ChildItem $Root -Recurse -File |
    Where-Object {
        $rel = $_.FullName.Substring($Root.Length).TrimStart('\')
        $rel -notmatch '^\.git\\'        -and
        $rel -notmatch '^发布\\'          -and
        $rel -notmatch '^\.localcraft\\'  -and
        $rel -notmatch '^测试数据\\'       -and
        # dist\ 是可执行文件，单独打进「程序包」；源码包里不要混进二进制
        $rel -notmatch '^dist\\'          -and
        ($rel -notmatch '^build\\' -or $rel -eq 'build\build.ps1') -and
        $_.Name -ne 'portable.marker'
    }
Ok "源码 $($srcFiles.Count) 个文件，共 $(Mb (($srcFiles | Measure-Object -Property Length -Sum).Sum)) MB"

# ---------------------------------------------------------------- 3. 建目录
Step "3/6  建立归档目录"

if ($DryRun) {
    Info "（预演）会建 发布\v$Version\"
} else {
    if (Test-Path $Tag) { Remove-Item $Tag -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Tag | Out-Null
    Ok "已建立"
}

# ---------------------------------------------------------------- 4. 打包
Step "4/6  打包"

$zipSrc = Join-Path $Tag "LocalCraft-$Version-源码.zip"
$zipBin = Join-Path $Tag "LocalCraft-$Version-程序.zip"

if ($DryRun) {
    Info "（预演）源码包：$($srcFiles.Count) 个文件"
    Info "（预演）程序包：dist\ 下的可运行文件"
} else {
    # --- 源码包 ---
    # Compress-Archive 一次塞太多文件会慢且吃内存，分两批：先目录结构，再零散文件
    $tmp = Join-Path $env:TEMP "lc-src-$Version"
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    foreach ($f in $srcFiles) {
        $rel = $f.FullName.Substring($Root.Length).TrimStart('\')
        $dst = Join-Path $tmp $rel
        $dir = Split-Path $dst -Parent
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
        Copy-Item $f.FullName $dst -Force
    }
    Compress-Archive -Path (Join-Path $tmp '*') -DestinationPath $zipSrc -Force
    Remove-Item $tmp -Recurse -Force
    Ok "源码包  $(Mb (Get-Item $zipSrc).Length) MB"

    # --- 程序包 ---
    Compress-Archive -Path (Join-Path $DistDir '*') -DestinationPath $zipBin -Force
    Ok "程序包  $(Mb (Get-Item $zipBin).Length) MB"
}

# ---------------------------------------------------------------- 5. 快照文档
Step "5/6  保存介绍 / 日志 / 教程快照"

if ($DryRun) {
    Info "（预演）会复制 README.md、CHANGELOG.md、教程.md、docs\"
} else {
    foreach ($n in @("README.md", "CHANGELOG.md", "教程.md")) {
        $s = Join-Path $Root $n
        if (Test-Path $s) { Copy-Item $s (Join-Path $Tag $n) -Force; Ok $n }
    }
    $docs = Join-Path $Root "docs"
    if (Test-Path $docs) {
        Copy-Item $docs (Join-Path $Tag "docs") -Recurse -Force
        Ok "docs\（架构文档与截图）"
    }

    # 源码目录清单
    $lines = @("LocalCraft $Version 源码包内容", "==================================================", "")
    $srcFiles | Sort-Object { $_.FullName.Substring($Root.Length) } | ForEach-Object {
        $rel = $_.FullName.Substring($Root.Length).TrimStart('\')
        $lines += ("{0,10:N0}  {1}" -f $_.Length, $rel)
    }
    $lines += @("", "共 $($srcFiles.Count) 个文件，$(Mb (($srcFiles | Measure-Object -Property Length -Sum).Sum)) MB")
    [System.IO.File]::WriteAllLines((Join-Path $Tag "源码目录.txt"), $lines,
                                    (New-Object System.Text.UTF8Encoding $true))
    Ok "源码目录.txt"
}

# ---------------------------------------------------------------- 6. 版本信息
Step "6/6  写版本信息"

if (-not $DryRun) {
    $exe = Join-Path $DistDir "LocalCraft.exe"
    $vi  = (Get-Item $exe).VersionInfo
    $hExe = (Get-FileHash $exe -Algorithm SHA256).Hash
    $hZip = (Get-FileHash $zipBin -Algorithm SHA256).Hash
    $hSrc = (Get-FileHash $zipSrc -Algorithm SHA256).Hash

    $info = @"
LocalCraft $Version 版本归档
==================================================

归档时间   $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
版本号     $Version（来自 src\app.rc 的 FILEVERSION）
发布人     $($vi.CompanyName)

程序
--------------------------------------------------
文件版本   $($vi.FileVersion)
产品名称   $($vi.ProductName)
文件说明   $($vi.FileDescription)
exe 大小   $('{0:N0}' -f (Get-Item $exe).Length) 字节
exe SHA256 $hExe

包
--------------------------------------------------
LocalCraft-$Version-程序.zip
           $('{0:N0}' -f (Get-Item $zipBin).Length) 字节
           SHA256 $hZip

LocalCraft-$Version-源码.zip
           $($srcFiles.Count) 个文件
           $('{0:N0}' -f (Get-Item $zipSrc).Length) 字节
           SHA256 $hSrc

怎么用
--------------------------------------------------
1. 解压「程序.zip」，双击 LocalCraft.exe
2. WebView2Loader.dll 必须和 exe 放在一起
3. 如果 Windows 弹「已保护你的电脑」，先双击同目录的「解除锁定.bat」

源码怎么编译
--------------------------------------------------
需要 MinGW-w64 gcc，然后：
    powershell -ExecutionPolicy Bypass -File build\build.ps1 -Tests
"@
    [System.IO.File]::WriteAllText((Join-Path $Tag "版本信息.txt"), $info,
                                   (New-Object System.Text.UTF8Encoding $true))
    Ok "版本信息.txt"
}

# ---------------------------------------------------------------- 外层包
if (-not $NoOuterZip -and -not $DryRun) {
    if (Test-Path $OuterZip) { Remove-Item $OuterZip -Force }
    Compress-Archive -Path $Tag -DestinationPath $OuterZip -Force
    Write-Host ""
    Ok "外层整包  发布\v$Version.zip  $(Mb (Get-Item $OuterZip).Length) MB"
}

Write-Host ""
Write-Host "  ==================================================" -ForegroundColor Green
if ($DryRun) { Write-Host "  预演结束，没有写任何文件。" -ForegroundColor Yellow }
else         { Write-Host "  已归档：发布\v$Version\" -ForegroundColor Green }

# 列出已有的所有版本，让人一眼看到积累
if (-not $DryRun -and (Test-Path $OutBase)) {
    $vers = Get-ChildItem $OutBase -Directory | Sort-Object Name
    Write-Host ""
    Write-Host "  目前已归档的版本：" -ForegroundColor White
    foreach ($v in $vers) {
        $sz = (Get-ChildItem $v.FullName -File | Measure-Object -Property Length -Sum).Sum
        Write-Host ("    {0,-10} {1,8} MB" -f $v.Name, (Mb $sz))
    }
}
Write-Host ""

<#
.SYNOPSIS
    把 LocalCraft 推送到 GitHub。

.DESCRIPTION
    一条命令走完：初始化仓库 → 暂存 → 提交 → 设置远程 → 推送。

    会顺带处理一个常见的坑：在 GitHub 网页上建仓库时如果勾了
    "Add a README file"，远程就已经有一个提交了，本地又是全新历史，
    直接推会被拒（non-fast-forward）。脚本会先探测远程状态并告诉你该怎么办。

.PARAMETER RepoUrl
    远程仓库地址，默认 https://github.com/Sudfry/LocalCraft.git

.PARAMETER Branch
    分支名，默认 main

.PARAMETER Message
    提交信息。不填则自动按时间生成。

.PARAMETER Force
    远程已有内容时强制覆盖。**会丢掉远程上原有的提交**，
    仅在你确定要"以本地为准"时使用。

.PARAMETER DryRun
    只显示将要执行的操作，不真的提交和推送。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File 上传到GitHub.ps1

.EXAMPLE
    # 先看看会做什么
    powershell -ExecutionPolicy Bypass -File 上传到GitHub.ps1 -DryRun

.EXAMPLE
    # 远程已有 README，以本地为准覆盖掉
    powershell -ExecutionPolicy Bypass -File 上传到GitHub.ps1 -Force

.EXAMPLE
    # 明知不用记日志（比如只改了注释），跳过日志检查
    powershell -ExecutionPolicy Bypass -File 上传到GitHub.ps1 -NoChangelogCheck
#>
[CmdletBinding()]
param(
    [string]$RepoUrl = "https://github.com/Sudfry/LocalCraft.git",
    [string]$Branch  = "main",
    [string]$Message = "",
    [switch]$Force,
    [switch]$DryRun
)

# 这里刻意用 Continue 而不是 Stop。
# 脚本要频繁调用 git，而 git 把很多"不算错误"的情况也写到 stderr
# （比如 rev-parse 在还没有提交时的提示）。
# 用 Stop 会让这些正常情况直接中断脚本，所以改成不自动中止，
# 每一步显式检查 $LASTEXITCODE。
$ErrorActionPreference = "Continue"

# 脚本放在仓库根目录，用它的位置定位仓库
$RepoRoot = $PSScriptRoot
if (-not $RepoRoot) { $RepoRoot = (Get-Location).Path }

function Step($n, $text) {
    Write-Host ""
    Write-Host "[$n] $text" -ForegroundColor Cyan
}
function Ok($text)   { Write-Host "    $text" -ForegroundColor Green }
function Info($text) { Write-Host "    $text" -ForegroundColor Gray }
function Warn($text) { Write-Host "    $text" -ForegroundColor Yellow }
function Die($text)  { Write-Host ""; Write-Host "  错误：$text" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "  LocalCraft → GitHub" -ForegroundColor White
Write-Host "  =================================================="
Write-Host "  仓库目录：$RepoRoot"
Write-Host "  远程地址：$RepoUrl"
Write-Host "  分支    ：$Branch"
if ($DryRun) { Write-Host "  模式    ：预演（不会真的提交/推送）" -ForegroundColor Yellow }

# ---------------------------------------------------------------- 1. 环境
Step 1 "检查环境"

$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) {
    Die @"
找不到 git。请先安装 Git for Windows：https://git-scm.com/download/win
装完后重新打开一个终端再运行本脚本。
"@
}
Info "git: $($git.Source)"

$userName  = (& git config --global user.name)  2>$null
$userEmail = (& git config --global user.email) 2>$null
if (-not $userName -or -not $userEmail) {
    Die @"
还没有配置 git 身份，GitHub 会拒绝这次提交。

请先执行（把邮箱换成你 GitHub 账号绑定的那个）：
    git config --global user.name  "Sudfry"
    git config --global user.email "你的邮箱@example.com"
"@
}
Ok "提交身份：$userName <$userEmail>"

if ($RepoUrl -match 'github\.com[:/]([^/]+)/([^/\.]+)') {
    Info "仓库：$($matches[1])/$($matches[2])"
}

# ---------------------------------------------------------------- 2. 初始化
Step 2 "初始化本地仓库"

if (Test-Path (Join-Path $RepoRoot ".git")) {
    Ok "已经是 git 仓库"
} else {
    if ($DryRun) { Info "（预演）会执行 git init" }
    else {
        & git -C $RepoRoot init -b $Branch 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { & git -C $RepoRoot init 2>&1 | Out-Null }
        Ok "已执行 git init"
    }
}

# ---------------------------------------------------------------- 2.5 安全闸
#
# 这一步是**必须的**，不是保险起见。
#
# git 找仓库的方式是"从当前目录逐级向上找 .git"。用户主目录
# C:\Users\<你> 下面很可能已经有一个 .git（某些 CLI 工具会自动创建）。
# 一旦 LocalCraft 自己还没有 .git，git 就会一路向上认到主目录，
# 于是 git add -A 会把整个主目录加进来 ——
# 里面可能有 .bash_history、.claude.json、SSH 私钥、浏览器数据。
# 推到公开仓库之后这些内容就永久留在提交历史里了，删都删不干净。
#
# 所以这里强制核对 git 眼中的仓库根，不是我们要的目录就直接退出。
if (-not $DryRun) {
    $top  = (& git -C $RepoRoot rev-parse --show-toplevel 2>$null | Select-Object -First 1)
    if ($top) { $top = ($top.Trim() -replace '/', '\').TrimEnd('\') }
    $want = ($RepoRoot -replace '/', '\').TrimEnd('\')

    if ($top -ne $want) {
        Write-Host ""
        Warn "git 认为仓库根目录是："
        Warn "    $top"
        Warn "而本脚本要操作的目录是："
        Warn "    $want"
        Write-Host ""
        Warn "两者不一致，说明 git 往上找到了别的 .git 目录。"
        Warn "如果就这样继续，git add -A 会把那个目录的内容全部加进来 ——"
        Warn "很可能包含你的个人文件，而这是要推到公开仓库的。"
        Write-Host ""
        Write-Host "  多半是因为主目录下有 .git：$HOME\.git" -ForegroundColor White
        Write-Host "  确认那个仓库不是你需要的，就先改名备份（不会删数据）：" -ForegroundColor White
        Write-Host "      Rename-Item `"$HOME\.git`" `".git.bak`"" -ForegroundColor Cyan
        Write-Host "  然后重新运行本脚本。" -ForegroundColor White
        Write-Host ""
        Die "仓库根目录校验未通过，已中止（没有暂存任何文件）"
    }
    Ok "仓库根校验通过：$top"
} else {
    # 预演模式不会执行 git init，所以这里只能提示、不能判定。
    # 但如果上级目录里已经有 .git，值得先说一声。
    if (-not (Test-Path (Join-Path $RepoRoot ".git"))) {
        $upGit = $null
        $d = Split-Path $RepoRoot -Parent
        while ($d) {
            if (Test-Path (Join-Path $d ".git")) { $upGit = $d; break }
            $parent = Split-Path $d -Parent
            if (-not $parent -or $parent -eq $d) { break }
            $d = $parent
        }
        if ($upGit) {
            Warn "上级目录有 .git：$upGit"
            Warn "正式运行时会先在本目录 git init，并在暂存前强制核对仓库根，不会误加那个目录。"
        }
    }
}

# git 默认把非 ASCII 文件名转义成 \344\270\212 这种形式，看着很难受。
# 这是仓库级设置，不影响全局配置。
& git -C $RepoRoot config core.quotepath false 2>$null | Out-Null
# ---------------------------------------------------------------- 3. 暂存
Step 3 "暂存文件"

# 预演模式下如果本目录还没初始化，git status 会一路扫到上级那个 .git，
# 结果又慢又刷一堆无关文件。这种情况直接跳过统计。
$ownGit = Test-Path (Join-Path $RepoRoot ".git")
if (-not $ownGit -and $DryRun) {
    Info "（预演）本目录还没有 .git，跳过暂存统计"
    $status = $null
} else {
    $status = & git -C $RepoRoot status --porcelain 2>$null
}

if (-not $status) {
    Ok "没有需要提交的改动"
} else {
    $count = ($status | Measure-Object).Count
    Info "共 $count 项改动"
    # 只打印前若干条，避免刷屏
    $status | Select-Object -First 12 | ForEach-Object { Info "  $_" }
    if ($count -gt 12) { Info "  …还有 $($count - 12) 项" }

    if ($DryRun) { Info "（预演）会执行 git add -A" }
    else {
        & git -C $RepoRoot add -A
        Ok "已暂存"
    }
}

# ---------------------------------------------------------------- 3.2 分类清点
#
# 这个仓库里同时放三类东西，各自的性质完全不同，混在一起很容易出事：
#
#   源码 —— 要入库，别人 clone 下来编译
#   软件 —— 编译产物（dist/ 里的 exe + dll）。它**要入库**，因为这是给不想
#           编译的人的现成版本；但它必须和当前源码版本号一致，
#           否则会出现"README 说 2.1.0，下载的却是 2.0.0"这种对不上的情况
#   介绍 —— README / 教程 / CHANGELOG，要入库
#
# 所以这一步做两件事：把即将上传的东西按类列清楚，并核对软件版本号。
Step 3.2 "分类清点"

$catSrc = @(); $catBin = @(); $catDoc = @(); $catOther = @()
foreach ($line in $status) {
    $p = ($line -replace '^\s*\S+\s+', '').Trim()
    if ($p -match '^src/|^tests/|^tools/|^webui/|^third_party/|^build/') { $catSrc += $p }
    elseif ($p -match '^dist/') { $catBin += $p }
    elseif ($p -match '\.md$|^LICENSE|^docs/|\.ps1$') { $catDoc += $p }
    else { $catOther += $p }
}
if ($catSrc.Count)   { Info "源码  $($catSrc.Count) 项" }
if ($catBin.Count)   { Info "软件  $($catBin.Count) 项  ← dist/ 里的编译产物" }
if ($catDoc.Count)   { Info "介绍  $($catDoc.Count) 项  ← README / 教程 / 日志 / 脚本" }
if ($catOther.Count) { Info "其他  $($catOther.Count) 项" }

# 核对：src/app.rc 里写的版本号 vs dist 里那个 exe 的版本号
if (-not $DryRun) {
    $rcPath = Join-Path $RepoRoot "src\app.rc"
    $distExe = Join-Path $RepoRoot "dist\LocalCraft.exe"
    if ((Test-Path $rcPath) -and (Test-Path $distExe)) {
        $rcText = Get-Content $rcPath -Raw -Encoding UTF8
        $m = [regex]::Match($rcText, 'FILEVERSION\s+(\d+),(\d+),(\d+),(\d+)')
        if ($m.Success) {
            $srcVer = "{0}.{1}.{2}" -f $m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[3].Value
            $binVer = (Get-Item $distExe).VersionInfo.FileVersion
            if ($binVer -and $binVer.StartsWith($srcVer)) {
                Ok "版本一致：源码 $srcVer · dist 里的 exe $binVer"
            } else {
                Write-Host ""
                Warn "版本对不上："
                Warn "    src/app.rc 写的是  $srcVer"
                Warn "    dist 里的 exe 是   $binVer"
                Warn "说明 dist 里放的是旧版本，或者改了版本号之后没重新构建。"
                Write-Host ""
                Write-Host "  先重新构建并同步到 dist：" -ForegroundColor White
                Write-Host "      powershell -ExecutionPolicy Bypass -File build\build.ps1 -Clean -Tests" -ForegroundColor Cyan
                Write-Host "      Copy-Item build\LocalCraft.exe dist\LocalCraft.exe -Force" -ForegroundColor Cyan
                Write-Host "      Copy-Item build\WebView2Loader.dll dist\WebView2Loader.dll -Force" -ForegroundColor Cyan
                Write-Host ""
                if (-not $Force) {
                    Die "源码与软件版本不一致（确认要以现状上传就加 -Force）"
                }
                Warn "-Force 已指定，继续上传。"
            }
        }
    } else {
        if (-not (Test-Path $distExe)) {
            Warn "dist\LocalCraft.exe 不存在 —— 这次会上传一个没有现成软件版本的仓库"
        }
    }
}

# ---------------------------------------------------------------- 3.5 日志检查
#
# "每次更新都写日志"这件事，光靠自觉是做不到的 —— 忙起来就忘了。
# 所以在上传前拦一道：如果这次动了代码，但 CHANGELOG.md 没跟着改，就提醒一下。
#
# 只提醒、不硬拦：真有不需要记的改动（比如只调了个注释），
# 加 -NoChangelogCheck 跳过，或者直接回车继续。
$changelogPath = Join-Path $RepoRoot "CHANGELOG.md"

function Test-UnreleasedEmpty {
    if (-not (Test-Path $changelogPath)) { return $true }
    $txt = [System.IO.File]::ReadAllText($changelogPath, [System.Text.Encoding]::UTF8)
    $inBlock = $false
    foreach ($ln in ($txt -split "`r?`n")) {
        if ($ln -match '^##\s+\[' + [regex]::Escape('未发布') + '\]') { $inBlock = $true; continue }
        if ($inBlock -and ($ln -match '^##\s+\[' -or $ln -match '^---\s*$')) { break }
        if ($inBlock -and $ln -match '^\s*-\s+\S') { return $false }
    }
    return $true
}

# 改动里有没有"代码"（文档和图片不算，那些不必每条都记日志）
$changedNames = @()
if (-not $DryRun) {
    $changedNames = @(& git -C $RepoRoot status --porcelain 2>$null) |
                    ForEach-Object { ($_ -split '\s+', 2)[1] }
}
$codeTouched = @($changedNames | Where-Object {
    $_ -match '\.(c|h|rc|manifest|js|css|html|ps1|py|md)$' -and
    $_ -notmatch '^docs/' -and $_ -notmatch '^(README|CHANGELOG|LICENSE)'
})
$logTouched  = @($changedNames | Where-Object { $_ -match '^CHANGELOG\.md$' })

# 首次提交时不做这个检查：那时所有文件都是"新增"，
# 而 CHANGELOG 里已经用 1.0.0 之类的版本段落把这些内容写清楚了，
# 再要求往 [未发布] 里写一遍就成了误报。
$hasAnyCommit = $false
& git -C $RepoRoot rev-parse --verify --quiet HEAD 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { $hasAnyCommit = $true }

if (-not $DryRun -and -not $NoChangelogCheck -and $hasAnyCommit -and $codeTouched.Count -gt 0) {
    if ($logTouched.Count -eq 0 -or (Test-UnreleasedEmpty)) {
        Write-Host ""
        Warn "这次改了代码，但 CHANGELOG.md 没有跟上："
        $codeTouched | Select-Object -First 6 | ForEach-Object { Warn "  改动了 $_" }
        if ($codeTouched.Count -gt 6) { Warn "  …等共 $($codeTouched.Count) 个文件" }
        if ($logTouched.Count -gt 0 -and (Test-UnreleasedEmpty)) {
            Warn "  （CHANGELOG.md 动过了，但 [未发布] 区块还是空的）"
        }
        Write-Host ""
        Write-Host "  先记一条的话，另开一个窗口执行：" -ForegroundColor White
        Write-Host "      .\记录更新.ps1 -Added `"这次加了什么`" -Fixed `"修了什么`"" -ForegroundColor Cyan
        Write-Host ""
        Write-Host "  或者现在就补（记完按提示重新跑本脚本）？" -ForegroundColor White
        $ans = Read-Host "  直接回车 = 不管了继续上传；输入 y = 停下来去补日志"
        if ($ans -match '^[yY]') {
            Write-Host ""
            Info "已停下。补完日志后重新运行本脚本即可。"
            Write-Host ""
            exit 0
        }
        Warn "按你的选择继续上传（这次没记日志）"
    }
}

# ---------------------------------------------------------------- 4. 提交
Step 4 "提交"

if (-not $Message) {
    $Message = "更新 LocalCraft（$(Get-Date -Format 'yyyy-MM-dd HH:mm')）"
}

$hasHead = $false
& git -C $RepoRoot rev-parse --verify --quiet HEAD 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { $hasHead = $true }

if ($DryRun) {
    Info "（预演）会执行 git commit -m `"$Message`""
} else {
    & git -C $RepoRoot commit -m $Message 2>&1 | ForEach-Object { Info "  $_" }
    if ($LASTEXITCODE -ne 0) {
        Warn "没有新的改动可提交（或提交失败）"
    } else {
        Ok "已提交"
    }
}

# 确保分支名正确
if (-not $DryRun) { & git -C $RepoRoot branch -M $Branch 2>&1 | Out-Null }

# ---------------------------------------------------------------- 5. 远程
Step 5 "设置远程地址"

$origin = (& git -C $RepoRoot remote get-url origin) 2>$null
if ($origin -eq $RepoUrl) {
    Ok "origin 已指向该地址"
} else {
    if ($origin) {
        Warn "origin 原本是 $origin，改为 $RepoUrl"
        if (-not $DryRun) { & git -C $RepoRoot remote set-url origin $RepoUrl }
    } else {
        if (-not $DryRun) { & git -C $RepoRoot remote add origin $RepoUrl }
        Ok "已添加 origin"
    }
}

# ---------------------------------------------------------------- 6. 探测远程
Step 6 "探测远程仓库状态"

$remoteHasContent = $false
if ($DryRun) {
    Info "（预演）跳过远程探测"
} else {
    $probe = & git -C $RepoRoot ls-remote --heads origin 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Warn "连不上远程仓库。常见原因："
        Warn "  1. 没有网络，或公司网络/代理拦了 github.com"
        Warn "  2. 仓库地址不对，或还没有在 GitHub 上创建"
        Warn "  3. 需要登录凭据（私有仓库）"
        Write-Host ""
        Die "无法访问 $RepoUrl"
    }
    if ($probe) {
        $remoteHasContent = $true
        Info "远程已有分支："
        $probe | ForEach-Object { Info "  $_" }
    } else {
        Ok "远程是空仓库，可以直接推"
    }
}

# 远程有内容时，本地是全新历史的话会被拒
$needForce = $false
if ($remoteHasContent -and -not $DryRun) {
    # 完整取一次远程（不用 --depth=1：浅历史会让 merge-base 误判成"没有共同祖先"）
    & git -C $RepoRoot fetch origin 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        & git -C $RepoRoot merge-base HEAD "origin/$Branch" 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $needForce = $true
        }
    }
}

if ($needForce -and -not $Force) {
    Write-Host ""
    Warn "远程仓库已经有提交（多半是建仓库时勾了「Add a README file」），"
    Warn "而本地是一段全新的历史，两者没有共同祖先 —— 直接推会被拒绝。"
    Write-Host ""
    Write-Host "  两个选择：" -ForegroundColor White
    Write-Host "    甲）以本地为准，覆盖远程："
    Write-Host "        powershell -ExecutionPolicy Bypass -File 上传到GitHub.ps1 -Force" -ForegroundColor Cyan
    Write-Host "        （远程原有的提交会丢失。如果那只是自动生成的 README，无所谓。）"
    Write-Host ""
    Write-Host "    乙）先在 GitHub 网页上把那个自动生成的 README 删掉，再重新运行本脚本。"
    Write-Host ""
    exit 1
}

# ---------------------------------------------------------------- 7. 推送
Step 7 "推送到 GitHub"

if ($DryRun) {
    Info "（预演）会执行：git push -u origin $Branch$(if ($Force -or $needForce) { ' --force' })"
    Write-Host ""
    Ok "预演结束，没有做任何改动。"
    exit 0
}

$pushArgs = @("-C", $RepoRoot, "push", "-u", "origin", $Branch)
if ($Force -or $needForce) {
    Warn "使用 --force 覆盖远程分支"
    $pushArgs += "--force"
}

& git @pushArgs 2>&1 | ForEach-Object { Info "  $_" }

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Warn "推送失败。如果提示的是认证问题："
    Warn "  · 现在 GitHub 不能用账号密码推送，需要用 Personal Access Token 或个人访问令牌"
    Warn "  · 到 https://github.com/settings/tokens 生成一个（勾选 repo 权限）"
    Warn "  · 推送时用户名填 GitHub 用户名，密码那一栏粘贴令牌"
    Warn "  · 或者装一个 Git Credential Manager，首次推送会弹浏览器登录"
    Write-Host ""
    Die "git push 返回 $LASTEXITCODE"
}

Write-Host ""
Write-Host "  ==================================================" -ForegroundColor Green
Ok "推送成功"
Write-Host ""
Write-Host "  仓库地址：https://github.com/Sudfry/LocalCraft" -ForegroundColor White
Write-Host ""

# 顺便提示还有哪些没做
$tag = (& git -C $RepoRoot tag --list) 2>$null
if (-not $tag) {
    Write-Host "  还没有打 tag。想发一个正式版本的话：" -ForegroundColor Gray
    Write-Host "    git tag -a v1.0 -m `"首个版本`" ; git push origin v1.0" -ForegroundColor Gray
    Write-Host ""
}

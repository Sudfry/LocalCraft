# ==========================================================================
# LocalCraft 构建脚本
#   用法：  powershell -ExecutionPolicy Bypass -File build\build.ps1
#           powershell -ExecutionPolicy Bypass -File build\build.ps1 -Clean
#           powershell -ExecutionPolicy Bypass -File build\build.ps1 -Tests
#           powershell -ExecutionPolicy Bypass -File build\build.ps1 -Console
#
# 只依赖 MinGW-w64 gcc，不需要 MSVC / CMake / Python / Node。
# ==========================================================================
param(
    [switch]$Clean,
    [switch]$Tests,
    [switch]$Console,
    [int]$TestTimeoutSec = 90
)

$ErrorActionPreference = "Stop"

$Root    = Split-Path -Parent $PSScriptRoot
$SrcDir  = Join-Path $Root "src"
$ObjDir  = Join-Path $Root "build\obj"
$BinDir  = Join-Path $Root "build"
$TestDir = Join-Path $Root "tests"

# ---- 找 gcc -------------------------------------------------------------
$Gcc = (Get-Command gcc -ErrorAction SilentlyContinue).Source
if (-not $Gcc) {
    foreach ($c in @("D:\environmentedit\mingw64\bin\gcc.exe",
                     "C:\mingw64\bin\gcc.exe",
                     "C:\msys64\mingw64\bin\gcc.exe",
                     "C:\TDM-GCC-64\bin\gcc.exe")) {
        if (Test-Path $c) { $Gcc = $c; break }
    }
}
if (-not $Gcc) { throw "找不到 gcc。请把 MinGW-w64 的 bin 目录加入 PATH。" }
$GccDir = Split-Path -Parent $Gcc
$CC = Join-Path $GccDir "gcc.exe"

# ---- 编译参数 -----------------------------------------------------------
$CFlags = @(
    "-std=c11",
    "-O2",
    "-municode",
    "-D__USE_MINGW_ANSI_STDIO=1",
    "-DUNICODE", "-D_UNICODE",
    "-D_WIN32_WINNT=0x0601", "-DWINVER=0x0601",
    "-Isrc",
    "-Wall", "-Wextra",
    "-Wno-unused-parameter",
    "-fno-strict-aliasing"
)
$LdFlags = @("-mwindows", "-municode", "-static", "-static-libgcc",
             "-lws2_32", "-lshlwapi", "-lshell32", "-lole32", "-luuid",
             "-lcomctl32", "-lgdi32", "-lgdiplus", "-luser32", "-ladvapi32", "-lcrypt32")

if ($Clean) {
    # 只清理生成物，绝不删除脚本自身所在目录
    foreach ($d in @("obj", "regexwork", "testtmp")) {
        $p = Join-Path $BinDir $d
        if (Test-Path $p) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }
    }
    Get-ChildItem $BinDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @(".exe", ".log", ".err", ".o") } |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Write-Host "已清理生成物。" -ForegroundColor DarkGray
}

New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

# ---- 1) 生成内嵌界面资源 ------------------------------------------------
Write-Host "[1/4] 打包界面资源 …" -ForegroundColor Cyan
# embed 是普通控制台工具（int main），不能用 -municode
$toolFlags = $CFlags | Where-Object { $_ -ne "-municode" }
& $CC @toolFlags -o (Join-Path $ObjDir "embed.exe") (Join-Path $Root "tools\embed.c")
if ($LASTEXITCODE -ne 0) { throw "embed 工具编译失败" }
Push-Location $Root
& (Join-Path $ObjDir "embed.exe") "webui" (Join-Path $SrcDir "webui_assets.c")
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "界面资源打包失败" }
Pop-Location

# ---- 2) 编译所有源文件 --------------------------------------------------
Write-Host "[2/4] 编译源码 …" -ForegroundColor Cyan
$sources = Get-ChildItem (Join-Path $SrcDir "*.c") | ForEach-Object { $_.FullName }
$objects = @()
$warned = 0
foreach ($src in $sources) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj  = Join-Path $ObjDir "$name.o"
    Write-Host "      $name.c" -ForegroundColor DarkGray
    # 原生命令的 stderr 直接落文件：PS 5.1 里 2>&1 会把警告变成终止错误
    $warn = Join-Path $ObjDir "$name.warn"
    if (Test-Path $warn) { Remove-Item $warn -Force }
    & $CC @CFlags -c $src -o $obj 2>$warn
    if ($LASTEXITCODE -ne 0) {
        if (Test-Path $warn) { Get-Content $warn | Write-Host -ForegroundColor Red }
        throw "编译失败：$src"
    }
    if (Test-Path $warn) {
        $wtxt = Get-Content $warn -Raw
        if ($wtxt -and $wtxt.Trim()) {
            Write-Host $wtxt.Trim() -ForegroundColor Yellow
            $warned++
        }
    }
    $objects += $obj
}

# ---- 3) 链接 ------------------------------------------------------------
Write-Host "[3/4] 链接 …" -ForegroundColor Cyan
# ---- 资源：应用清单 + 版本信息 -----------------------------------------
# 清单里引用了 Common-Controls 6.0，没有它所有控件都会按 Win95 老样式绘制。
$resFile = Join-Path $ObjDir "app.res"
$rcSrc = Join-Path $SrcDir "app.rc"
if (Test-Path $rcSrc) {
    Write-Host "  编译资源（清单 / 版本信息）…" -ForegroundColor DarkGray
    & windres -I $SrcDir $rcSrc -O coff -o $resFile 2>$null
    if ($LASTEXITCODE -eq 0 -and (Test-Path $resFile)) {
        $objects += $resFile
    } else {
        Write-Host "  资源编译失败（界面会退化成旧样式，功能不受影响）" -ForegroundColor Yellow
    }
}
$outExe = Join-Path $BinDir "LocalCraft.exe"
$ld = $LdFlags
if ($Console) {
    $ld = $ld | Where-Object { $_ -ne "-mwindows" } | Where-Object { $_ -ne "-municode" }
    $ld += "-mconsole"
}
& $CC -o $outExe @objects @ld
if ($LASTEXITCODE -ne 0) { throw "链接失败" }

# ---- 把 WebView2Loader.dll 放到 exe 旁边 ---------------------------------
# 它只负责把系统里装好的 WebView2 运行库拉起来；没有它就只能退回纯控件界面。
$loader = Join-Path (Join-Path $Root "third_party") "WebView2Loader.dll"
if (Test-Path $loader) {
    Copy-Item $loader (Join-Path $BinDir "WebView2Loader.dll") -Force
}
$size = (Get-Item $outExe).Length / 1MB
$warnNote = if ($warned) { "，$warned 个文件有警告" } else { "，无警告" }
Write-Host ("[4/4] 完成：{0}  ({1:N2} MB{2})" -f $outExe, $size, $warnNote) -ForegroundColor Green

# ---- 可选：跑测试 -------------------------------------------------------
if ($Tests) {
    # 测试夹具：test_zip 需要一个**外部工具产出**的 deflate 压缩包来验证解压器
    # （自己的写入器只能产生 STORED，验证不了 Huffman/LZ77）。这里用系统自带的
    # Compress-Archive 生成，纯 PowerShell、不联网、不需要任何第三方工具。
    $zipwork = Join-Path $BinDir "zipwork"
    $realzip = Join-Path $zipwork "real.zip"
    if (-not (Test-Path $realzip)) {
        Write-Host "  生成 ZIP 测试夹具（Compress-Archive）…" -ForegroundColor DarkGray
        New-Item -ItemType Directory -Force -Path $zipwork | Out-Null
        $stage = Join-Path $zipwork "stage"
        if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $stage | Out-Null
        # 三个文件的布局是 test_zip 约定好的，别改名字：
        #   a.txt 极小；b.txt 10 万个 'X'；c.txt 100 万个 'A'
        # 三者都高度可压缩，压完只有一千多字节 —— 解压时能覆盖
        # 动态 Huffman、长 LZ77 匹配和全部长度/距离码。
        [System.IO.File]::WriteAllText((Join-Path $stage "a.txt"), "hello zip")
        [System.IO.File]::WriteAllText((Join-Path $stage "b.txt"), ("X" * 100000))
        [System.IO.File]::WriteAllText((Join-Path $stage "c.txt"), ("A" * 1048576))
        Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $realzip -Force
        Remove-Item $stage -Recurse -Force
        Write-Host ("  夹具已生成：{0} ({1:N0} 字节)" -f $realzip, (Get-Item $realzip).Length) -ForegroundColor DarkGray
    }
    Write-Host "`n运行测试 …" -ForegroundColor Cyan
    $testSources = Get-ChildItem (Join-Path $TestDir "test_*.c") -ErrorAction SilentlyContinue |
                   ForEach-Object { $_.FullName }
    if (-not $testSources) { Write-Host "  没有测试文件。" -ForegroundColor Yellow }

    # 测试是控制台程序，去掉宽字符入口开关
    $testFlags = $CFlags | Where-Object { $_ -ne "-municode" }
    # 排除 main.c（它自带 wWinMain），测试自带 main
    $tobjs = @($objects | Where-Object { $_ -notmatch "\\main\.o$" })
    $testLibs = @("-lws2_32","-lshlwapi","-lshell32","-lole32","-luuid",
                  "-lcomctl32","-lgdi32","-lgdiplus","-luser32","-ladvapi32","-lcrypt32",
                  "-static","-static-libgcc")

    $failed = 0
    $summary = @()
    foreach ($t in $testSources) {
        $tn = [System.IO.Path]::GetFileNameWithoutExtension($t)
        $texe = Join-Path $ObjDir "$tn.exe"
        $tlog = Join-Path $ObjDir "$tn.log"
        $terr = Join-Path $ObjDir "$tn.err"

        if (Test-Path $terr) { Remove-Item $terr -Force }
        & $CC @testFlags -o $texe $t $tobjs @testLibs 2>$terr
        if ($LASTEXITCODE -ne 0) {
            if (Test-Path $terr) { Get-Content $terr | Write-Host -ForegroundColor Red }
            Write-Host "  x $tn  compile failed" -ForegroundColor Red
            $summary += [pscustomobject]@{ Test = $tn; Result = "compile failed" }
            $failed++
            continue
        }

        # 带超时运行：某个测试卡死不能把整个构建拖住。
        # 用 System.Diagnostics.Process（Start-Process -PassThru 的 ExitCode 在
        # 重定向输出时不可靠），套一层 cmd.exe 把 stdout/stderr 落文件，
        # 退出码由 cmd 原样透传。
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = "$env:ComSpec"
        $psi.Arguments = "/c `"`"$texe`" > `"$tlog`" 2> `"$terr`"`""
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $proc = [System.Diagnostics.Process]::Start($psi)
        $timedOut = -not $proc.WaitForExit($TestTimeoutSec * 1000)
        if ($timedOut) {
            try { $proc.Kill() } catch {}
            Write-Host "  x $tn  TIMEOUT (${TestTimeoutSec}s, killed)" -ForegroundColor Red
            $summary += [pscustomobject]@{ Test = $tn; Result = "timeout" }
            $failed++
            continue
        }
        $code = $proc.ExitCode

        if (Test-Path $tlog) {
            $body = Get-Content $tlog -Raw -ErrorAction SilentlyContinue
            if ($body) { Write-Host $body.TrimEnd() }
        }
        if (Test-Path $terr) {
            $eb = Get-Content $terr -Raw -ErrorAction SilentlyContinue
            if ($eb -and $eb.Trim()) { Write-Host $eb.TrimEnd() -ForegroundColor DarkGray }
        }

        if ($code -ne 0) {
            Write-Host "  x $tn  FAILED (exit $code)" -ForegroundColor Red
            $summary += [pscustomobject]@{ Test = $tn; Result = "failed" }
            $failed++
        } else {
            Write-Host "  v $tn  passed" -ForegroundColor Green
            $summary += [pscustomobject]@{ Test = $tn; Result = "passed" }
        }
    }

    Write-Host ""
    $summary | Format-Table -AutoSize | Out-String | Write-Host
    if ($failed -gt 0) {
        Write-Host "$failed 个测试未通过。" -ForegroundColor Red
        exit 1
    }
    Write-Host "所有测试通过。" -ForegroundColor Green
}

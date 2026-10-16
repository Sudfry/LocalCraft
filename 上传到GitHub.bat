@echo off
rem ====================================================================
rem  用 UTF-8 代码页（65001）。
rem
rem  为什么不用 936：脚本内部要调用 git，而 git 输出的是 UTF-8。
rem  如果控制台是 936，PowerShell 会按 GBK 去解 git 的输出，
rem  中文文件名就变成「涓婁紶鍒癎itHub.bat」这种乱码。
rem  整个流程统一用 UTF-8 就没有这个错配。
rem
rem  这个 .bat 存成**不带 BOM 的 UTF-8**：cmd 遇到 BOM 会在第一行报错。
rem ====================================================================
chcp 65001 >nul 2>&1
setlocal
cd /d "%~dp0"

echo.
echo   LocalCraft  推送到 GitHub
echo   ==================================================
echo.

rem ---------------------------------------------------------------
rem 为什么要有这个 .bat
rem
rem 直接双击 .ps1、或者用 .\xxx.ps1 运行，会被 PowerShell 的执行策略拦住：
rem   「未对文件进行数字签名，无法在当前系统上运行该脚本」
rem
rem 这不是脚本有问题，是 Windows 对 .ps1 的默认防护。
rem 下面的 -ExecutionPolicy Bypass 只对这一次调用生效：
rem   - 不修改系统设置
rem   - 不影响其它脚本
rem   - 关掉这个窗口就没了
rem ---------------------------------------------------------------

set PSARGS=%*
if "%PSARGS%"=="" set PSARGS=-DryRun

echo   参数: %PSARGS%
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0上传到GitHub.ps1" %PSARGS%
set RC=%ERRORLEVEL%

echo.
if not "%RC%"=="0" (
  echo   [结束] 有错误，往上翻看原因。
) else (
  echo   [结束] 完成。
)
echo.
pause

@echo off
chcp 936 >nul 2>&1
setlocal
title LocalCraft - 解除安全提示
set "LC_DIR=%~dp0"

echo.
echo   LocalCraft 解除锁定
echo   ==================================================
echo.
echo   把本文件夹里（不含子文件夹）所有文件的「网络来源」标记去掉。
echo   那个标记是 QQ / 微信 / 网盘 / 浏览器 传送文件时加的，
echo   Windows 看到它就会弹「已保护你的电脑」。
echo.
echo   只动这一个元数据标记，不改文件内容，也不碰系统设置。
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "$d=$env:LC_DIR; $n=0; Get-ChildItem -LiteralPath $d -File -ErrorAction SilentlyContinue | ForEach-Object { if (Get-Item -LiteralPath $_.FullName -Stream Zone.Identifier -ErrorAction SilentlyContinue) { Unblock-File -LiteralPath $_.FullName -ErrorAction SilentlyContinue; $n++; Write-Host ('   已解除 ' + $_.Name) } }; Write-Host '' ; Write-Host ('   共解除 ' + $n + ' 个文件')"

echo.
echo   ==================================================
echo   完成。现在双击 LocalCraft.exe 不会再弹安全提示。
echo.
pause

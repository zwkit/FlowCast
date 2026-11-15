@echo off
chcp 65001 >nul
cd /d "%~dp0.."

echo ============================================================
echo   编解码对比  (A = H.264 / B = H.265)
echo ============================================================
echo.
echo   请先在浏览器打开  tools\quality-card.html  并按 F11 全屏
echo   （重点看左上 8-12px 小字、右上白底黑字 —— H.265 的强项）
echo.
echo   3 秒后开始，共 3 轮，每轮 A 15 秒 / B 15 秒，自动切换。
echo   关掉本窗口即随时停止。
echo.
echo   已知差异：H.264 跑满 30fps；H.265 约 24fps（这颗 AMD
echo   编码器的上限），所以动鼠标时 H.265 会稍顿一点。
echo.
timeout /t 3 >nul

for /l %%i in (1,1,3) do (
  echo [第 %%i 轮]  >>>  A : H.264  <<<
  build\bin\flowcast.exe --host 192.168.0.201 --duration 15 2>nul
  timeout /t 2 >nul
  echo [第 %%i 轮]  >>>  B : H.265  <<<
  build\bin\flowcast.exe --host 192.168.0.201 --hevc --duration 15 2>nul
  timeout /t 2 >nul
)

echo.
echo 对比结束。想再来一轮直接回车。
pause

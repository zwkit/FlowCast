@echo off
chcp 65001 >nul
cd /d "%~dp0.."

echo ============================================================
echo   A/B 画质对比  (A = 4K 源原样发送,  B = GPU 缩到 1080p)
echo ============================================================
echo.
echo   请先在浏览器打开  tools\quality-card.html  并按 F11 全屏
echo   （重点看左上 8-12px 小字、右上白底黑字、左下 1px 竖线）
echo.
echo   3 秒后开始，共 3 轮，每轮 A 12 秒 / B 12 秒，自动切换。
echo   关掉本窗口即随时停止。
echo.
timeout /t 3 >nul

for /l %%i in (1,1,3) do (
  echo [第 %%i 轮]  >>>  A : 4K 源，3840x2160 原样发送  <<<
  build\bin\flowcast.exe --host 192.168.0.201 --max-width 0 --duration 12 2>nul
  timeout /t 2 >nul
  echo [第 %%i 轮]  >>>  B : GPU 缩到 1920x1080 再发送  <<<
  build\bin\flowcast.exe --host 192.168.0.201 --max-width 1920 --duration 12 2>nul
  timeout /t 2 >nul
)

echo.
echo 对比结束。想再来一轮直接回车。
pause

@echo off
cd /d "%~dp0"
echo === Serial ports ===
powershell -NoProfile -ExecutionPolicy Bypass -File ".\bm7701.ps1" -Command Ports
echo.
echo === Starting BM7701 web control on COM11, http://127.0.0.1:8771/  (Ctrl+C to stop) ===
powershell -NoProfile -ExecutionPolicy Bypass -File ".\bm7701.ps1" -Command Web -Port COM11 -HttpPort 8771 -LogPath "%TEMP%\bm7701-web.log"
pause

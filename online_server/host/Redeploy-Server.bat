@echo off
REM Double-click to install or update the Silent Hill Online server on the Linux
REM machine named in server.txt (host=...). You will be asked for its password
REM once. The first run also creates the service, so the server starts with the
REM machine and restarts itself after a crash.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0redeploy-server.ps1"
echo.
pause
